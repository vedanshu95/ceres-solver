// Ceres Solver - A fast non-linear least squares minimizer
// Copyright 2012 Google Inc. All rights reserved.
// http://code.google.com/p/ceres-solver/
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * Neither the name of Google Inc. nor the names of its contributors may be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
// Author: moll.markus@arcor.de (Markus Moll)

#include "ceres/polynomial_solver.h"

#include <glog/logging.h>
#include <cmath>
#include <cstddef>

#include "Eigen/Dense"
#include "ceres/internal/port.h"

namespace ceres {
namespace internal {
namespace {

// Balancing function as described by B. N. Parlett and C. Reinsch,
// "Balancing a Matrix for Calculation of Eigenvalues and Eigenvectors".
// In: Numerische Mathematik, Volume 13, Number 4 (1969), 293-304,
// Springer Berlin / Heidelberg. DOI: 10.1007/BF02165404
void BalanceCompanionMatrix(Matrix* companion_matrix_ptr) {
  CHECK_NOTNULL(companion_matrix_ptr);
  Matrix& companion_matrix = *companion_matrix_ptr;
  Matrix companion_matrix_offdiagonal = companion_matrix;
  companion_matrix_offdiagonal.diagonal().setZero();

  const int degree = companion_matrix.rows();

  // gamma <= 1 controls how much a change in the scaling has to
  // lower the 1-norm of the companion matrix to be accepted.
  //
  // gamma = 1 seems to lead to cycles (numerical issues?), so
  // we set it slightly lower.
  const double gamma = 0.9;

  // Greedily scale row/column pairs until there is no change.
  bool scaling_has_changed;
  do {
    scaling_has_changed = false;

    for (int i = 0; i < degree; ++i) {
      const double row_norm = companion_matrix_offdiagonal.row(i).lpNorm<1>();
      const double col_norm = companion_matrix_offdiagonal.col(i).lpNorm<1>();

      // Decompose row_norm/col_norm into mantissa * 2^exponent,
      // where 0.5 <= mantissa < 1. Discard mantissa (return value
      // of frexp), as only the exponent is needed.
      int exponent = 0;
      std::frexp(row_norm / col_norm, &exponent);
      exponent /= 2;

      if (exponent != 0) {
        const double scaled_col_norm = std::ldexp(col_norm, exponent);
        const double scaled_row_norm = std::ldexp(row_norm, -exponent);
        if (scaled_col_norm + scaled_row_norm < gamma * (col_norm + row_norm)) {
          // Accept the new scaling. (Multiplication by powers of 2 should not
          // introduce rounding errors (ignoring non-normalized numbers and
          // over- or underflow))
          scaling_has_changed = true;
          companion_matrix_offdiagonal.row(i) *= std::ldexp(1.0, -exponent);
          companion_matrix_offdiagonal.col(i) *= std::ldexp(1.0, exponent);
        }
      }
    }
  } while (scaling_has_changed);

  companion_matrix_offdiagonal.diagonal() = companion_matrix.diagonal();
  companion_matrix = companion_matrix_offdiagonal;
  VLOG(3) << "Balanced companion matrix is\n" << companion_matrix;
}

void BuildCompanionMatrix(const Vector& polynomial,
                          Matrix* companion_matrix_ptr) {
  CHECK_NOTNULL(companion_matrix_ptr);
  Matrix& companion_matrix = *companion_matrix_ptr;

  const int degree = polynomial.size() - 1;

  companion_matrix.resize(degree, degree);
  companion_matrix.setZero();
  companion_matrix.diagonal(-1).setOnes();
  companion_matrix.col(degree-1) = -polynomial.reverse().head(degree);
}

// Remove leading terms with zero coefficients.
Vector RemoveLeadingZeros(const Vector& polynomial_in) {
  int i = 0;
  while (i < (polynomial_in.size() - 1) && polynomial_in(i) == 0.0) {
    ++i;
  }
  return polynomial_in.tail(polynomial_in.size() - i);
}
}  // namespace

bool FindPolynomialRoots(const Vector& polynomial_in,
                         Vector* real,
                         Vector* imaginary) {
  const double epsilon = std::numeric_limits<double>::epsilon();

  if (polynomial_in.size() == 0) {
    LOG(ERROR) << "Invalid polynomial of size 0 passed to FindPolynomialRoots";
    return false;
  }

  Vector polynomial = RemoveLeadingZeros(polynomial_in);
  const int degree = polynomial.size() - 1;

  // Is the polynomial constant?
  if (degree == 0) {
    LOG(WARNING) << "Trying to extract roots from a constant "
                 << "polynomial in FindPolynomialRoots";
    return true;
  }

  // Divide by leading term
  const double leading_term = polynomial(0);
  polynomial /= leading_term;

  // Separately handle linear polynomials.
  if (degree == 1) {
    if (real != NULL) {
      real->resize(1);
      (*real)(0) = -polynomial(1);
    }
    if (imaginary != NULL) {
      imaginary->resize(1);
      imaginary->setZero();
    }
  }

  // The degree is now known to be at least 2.
  // Build and balance the companion matrix to the polynomial.
  Matrix companion_matrix(degree, degree);
  BuildCompanionMatrix(polynomial, &companion_matrix);
  BalanceCompanionMatrix(&companion_matrix);

  // Find its (complex) eigenvalues.
  Eigen::EigenSolver<Matrix> solver(companion_matrix,
                                    Eigen::EigenvaluesOnly);
  if (solver.info() != Eigen::Success) {
    LOG(ERROR) << "Failed to extract eigenvalues from companion matrix.";
    return false;
  }

  // Output roots
  if (real != NULL) {
    *real = solver.eigenvalues().real();
  } else {
    LOG(WARNING) << "NULL pointer passed as real argument to "
                 << "FindPolynomialRoots. Real parts of the roots will not "
                 << "be returned.";
  }
  if (imaginary != NULL) {
    *imaginary = solver.eigenvalues().imag();
  }
  return true;
}

}  // namespace internal
}  // namespace ceres
