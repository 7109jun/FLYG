// flyG - dynamic dense linear algebra: Matrix, linear systems, LU / QR / Cholesky / eigen / SVD
// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include <flyg/core/util.hpp>
#include <flyg/math/scalar.hpp>

namespace flyg::math {

template <Real T = double>
using VectorX = std::vector<T>;

/// Dense row-major matrix with runtime dimensions.
template <Real T = double>
class Matrix {
public:
    using value_type = T;

    Matrix() = default;
    Matrix(usize rows, usize cols, T fill = T(0)) : rows_(rows), cols_(cols), d_(core::checked_mul(rows, cols), fill) {}
    Matrix(std::initializer_list<std::initializer_list<T>> init) {
        rows_ = init.size();
        cols_ = rows_ == 0 ? 0 : init.begin()->size();
        d_.reserve(rows_ * cols_);
        for (const auto& row : init) {
            if (row.size() != cols_) throw MathException("Matrix: ragged initializer list");
            d_.insert(d_.end(), row.begin(), row.end());
        }
    }

    [[nodiscard]] static Matrix identity(usize n) {
        Matrix m(n, n);
        for (usize i = 0; i < n; ++i) m(i, i) = T(1);
        return m;
    }

    [[nodiscard]] usize rows() const noexcept { return rows_; }
    [[nodiscard]] usize cols() const noexcept { return cols_; }
    [[nodiscard]] bool empty() const noexcept { return d_.empty(); }
    [[nodiscard]] bool square() const noexcept { return rows_ == cols_; }
    [[nodiscard]] T* data() noexcept { return d_.data(); }
    [[nodiscard]] const T* data() const noexcept { return d_.data(); }

    T& operator()(usize r, usize c) noexcept { return d_[r * cols_ + c]; }
    const T& operator()(usize r, usize c) const noexcept { return d_[r * cols_ + c]; }
    T& at(usize r, usize c) {
        if (r >= rows_ || c >= cols_) throw MathException("Matrix index out of range");
        return d_[r * cols_ + c];
    }
    const T& at(usize r, usize c) const {
        if (r >= rows_ || c >= cols_) throw MathException("Matrix index out of range");
        return d_[r * cols_ + c];
    }

    [[nodiscard]] VectorX<T> row(usize r) const {
        if (r >= rows_) throw MathException("Matrix row out of range");
        return VectorX<T>(d_.begin() + static_cast<std::ptrdiff_t>(r * cols_), d_.begin() + static_cast<std::ptrdiff_t>((r + 1) * cols_));
    }
    [[nodiscard]] VectorX<T> col(usize c) const {
        if (c >= cols_) throw MathException("Matrix column out of range");
        VectorX<T> v(rows_);
        for (usize r = 0; r < rows_; ++r) v[r] = (*this)(r, c);
        return v;
    }

    friend bool operator==(const Matrix&, const Matrix&) = default;

    Matrix& operator+=(const Matrix& o) {
        require_same_shape(o, "operator+=");
        for (usize i = 0; i < d_.size(); ++i) d_[i] += o.d_[i];
        return *this;
    }
    Matrix& operator-=(const Matrix& o) {
        require_same_shape(o, "operator-=");
        for (usize i = 0; i < d_.size(); ++i) d_[i] -= o.d_[i];
        return *this;
    }
    Matrix& operator*=(T s) noexcept { for (T& v : d_) v *= s; return *this; }
    friend Matrix operator+(Matrix a, const Matrix& b) { return a += b; }
    friend Matrix operator-(Matrix a, const Matrix& b) { return a -= b; }
    friend Matrix operator*(Matrix a, T s) { return a *= s; }
    friend Matrix operator*(T s, Matrix a) { return a *= s; }

    friend Matrix operator*(const Matrix& a, const Matrix& b) {
        if (a.cols_ != b.rows_) throw MathException("Matrix multiply: inner dimensions differ");
        Matrix r(a.rows_, b.cols_);
        for (usize i = 0; i < a.rows_; ++i)
            for (usize k = 0; k < a.cols_; ++k) {
                const T aik = a(i, k);
                for (usize j = 0; j < b.cols_; ++j) r(i, j) += aik * b(k, j);
            }
        return r;
    }
    friend VectorX<T> operator*(const Matrix& a, const VectorX<T>& v) {
        if (a.cols_ != v.size()) throw MathException("Matrix * vector: dimension mismatch");
        VectorX<T> r(a.rows_, T(0));
        for (usize i = 0; i < a.rows_; ++i)
            for (usize j = 0; j < a.cols_; ++j) r[i] += a(i, j) * v[j];
        return r;
    }

    [[nodiscard]] Matrix transposed() const {
        Matrix t(cols_, rows_);
        for (usize i = 0; i < rows_; ++i)
            for (usize j = 0; j < cols_; ++j) t(j, i) = (*this)(i, j);
        return t;
    }

    [[nodiscard]] T frobenius_norm() const noexcept {
        T s = T(0);
        for (const T v : d_) s += v * v;
        return std::sqrt(s);
    }

    void swap_rows(usize a, usize b) noexcept {
        if (a == b) return;
        for (usize j = 0; j < cols_; ++j) std::swap((*this)(a, j), (*this)(b, j));
    }

private:
    void require_same_shape(const Matrix& o, const char* op) const {
        if (rows_ != o.rows_ || cols_ != o.cols_) throw MathException(std::string("Matrix ") + op + ": shape mismatch");
    }
    usize rows_ = 0, cols_ = 0;
    std::vector<T> d_;
};

using MatrixXf = Matrix<float>;
using MatrixXd = Matrix<double>;

template <Real T>
[[nodiscard]] bool approx_equal(const Matrix<T>& a, const Matrix<T>& b, T rel = default_rel_tol<T>, T abs_ = default_abs_tol<T>) noexcept {
    if (a.rows() != b.rows() || a.cols() != b.cols()) return false;
    for (usize i = 0; i < a.rows() * a.cols(); ++i)
        if (!approx_equal(a.data()[i], b.data()[i], rel, abs_)) return false;
    return true;
}

template <Real T>
[[nodiscard]] T dot(const VectorX<T>& a, const VectorX<T>& b) {
    if (a.size() != b.size()) throw MathException("dot: size mismatch");
    T s = T(0);
    for (usize i = 0; i < a.size(); ++i) s += a[i] * b[i];
    return s;
}
template <Real T> [[nodiscard]] T norm(const VectorX<T>& a) { return std::sqrt(dot(a, a)); }

// ============================================================================
// LU decomposition with partial pivoting: P*A = L*U
// ============================================================================
template <Real T>
class LuDecomposition {
public:
    /// Throws MathException for a non-square matrix. A singular matrix is accepted here
    /// (is_singular() reports it); solve()/inverse() throw for singular systems.
    explicit LuDecomposition(const Matrix<T>& a) : lu_(a), piv_(a.rows()) {
        if (!a.square()) throw MathException("LU: matrix must be square");
        const usize n = a.rows();
        std::iota(piv_.begin(), piv_.end(), usize{0});
        T scale = T(0);
        for (usize i = 0; i < n * n; ++i) scale = std::max(scale, std::fabs(a.data()[i]));
        tiny_ = scale * T(n == 0 ? 1 : n) * std::numeric_limits<T>::epsilon();
        for (usize k = 0; k < n; ++k) {
            usize p = k;
            T best = std::fabs(lu_(k, k));
            for (usize i = k + 1; i < n; ++i)
                if (std::fabs(lu_(i, k)) > best) { best = std::fabs(lu_(i, k)); p = i; }
            if (best <= tiny_) { singular_ = true; continue; }
            if (p != k) { lu_.swap_rows(k, p); std::swap(piv_[k], piv_[p]); sign_ = -sign_; }
            for (usize i = k + 1; i < n; ++i) {
                lu_(i, k) /= lu_(k, k);
                const T f = lu_(i, k);
                for (usize j = k + 1; j < n; ++j) lu_(i, j) -= f * lu_(k, j);
            }
        }
    }

    [[nodiscard]] bool is_singular() const noexcept { return singular_; }

    [[nodiscard]] T determinant() const noexcept {
        if (singular_) return T(0);
        T d = static_cast<T>(sign_);
        for (usize i = 0; i < lu_.rows(); ++i) d *= lu_(i, i);
        return d;
    }

    [[nodiscard]] VectorX<T> solve(const VectorX<T>& b) const {
        const usize n = lu_.rows();
        if (b.size() != n) throw MathException("LU solve: right-hand side size mismatch");
        if (singular_) throw MathException("LU solve: matrix is singular");
        VectorX<T> x(n);
        for (usize i = 0; i < n; ++i) x[i] = b[piv_[i]];
        for (usize i = 0; i < n; ++i)
            for (usize j = 0; j < i; ++j) x[i] -= lu_(i, j) * x[j];
        for (usize i = n; i-- > 0;) {
            for (usize j = i + 1; j < n; ++j) x[i] -= lu_(i, j) * x[j];
            x[i] /= lu_(i, i);
        }
        return x;
    }

    [[nodiscard]] Matrix<T> solve(const Matrix<T>& b) const {
        if (b.rows() != lu_.rows()) throw MathException("LU solve: right-hand side row mismatch");
        Matrix<T> x(b.rows(), b.cols());
        for (usize c = 0; c < b.cols(); ++c) {
            const VectorX<T> col = solve(b.col(c));
            for (usize r = 0; r < b.rows(); ++r) x(r, c) = col[r];
        }
        return x;
    }

    [[nodiscard]] Matrix<T> inverse() const { return solve(Matrix<T>::identity(lu_.rows())); }

    [[nodiscard]] Matrix<T> lower() const {
        const usize n = lu_.rows();
        Matrix<T> l(n, n);
        for (usize i = 0; i < n; ++i) {
            for (usize j = 0; j < i; ++j) l(i, j) = lu_(i, j);
            l(i, i) = T(1);
        }
        return l;
    }
    [[nodiscard]] Matrix<T> upper() const {
        const usize n = lu_.rows();
        Matrix<T> u(n, n);
        for (usize i = 0; i < n; ++i)
            for (usize j = i; j < n; ++j) u(i, j) = lu_(i, j);
        return u;
    }
    /// Row permutation: row i of P*A is row permutation()[i] of A.
    [[nodiscard]] const std::vector<usize>& permutation() const noexcept { return piv_; }
    [[nodiscard]] Matrix<T> permutation_matrix() const {
        const usize n = piv_.size();
        Matrix<T> p(n, n);
        for (usize i = 0; i < n; ++i) p(i, piv_[i]) = T(1);
        return p;
    }

private:
    Matrix<T> lu_;
    std::vector<usize> piv_;
    int sign_ = 1;
    bool singular_ = false;
    T tiny_ = 0;
};

// ============================================================================
// QR decomposition (Householder): A = Q*R, Q is m x m orthogonal, R is m x n upper triangular
// ============================================================================
template <Real T>
struct QrResult {
    Matrix<T> q;
    Matrix<T> r;
};

template <Real T>
[[nodiscard]] QrResult<T> qr_decompose(const Matrix<T>& a) {
    const usize m = a.rows(), n = a.cols();
    if (m == 0 || n == 0) throw MathException("QR: empty matrix");
    Matrix<T> r = a;
    Matrix<T> q = Matrix<T>::identity(m);
    const usize steps = std::min(m - 1, n);
    for (usize k = 0; k < steps; ++k) {
        T norm_x = T(0);
        for (usize i = k; i < m; ++i) norm_x += r(i, k) * r(i, k);
        norm_x = std::sqrt(norm_x);
        if (norm_x == T(0)) continue;
        const T alpha = r(k, k) > T(0) ? -norm_x : norm_x;
        VectorX<T> v(m, T(0));
        for (usize i = k; i < m; ++i) v[i] = r(i, k);
        v[k] -= alpha;
        T vnorm2 = T(0);
        for (usize i = k; i < m; ++i) vnorm2 += v[i] * v[i];
        if (vnorm2 == T(0)) continue;
        for (usize j = 0; j < n; ++j) {  // R = H R
            T s = T(0);
            for (usize i = k; i < m; ++i) s += v[i] * r(i, j);
            s = T(2) * s / vnorm2;
            for (usize i = k; i < m; ++i) r(i, j) -= s * v[i];
        }
        for (usize i = 0; i < m; ++i) {  // Q = Q H
            T s = T(0);
            for (usize j = k; j < m; ++j) s += q(i, j) * v[j];
            s = T(2) * s / vnorm2;
            for (usize j = k; j < m; ++j) q(i, j) -= s * v[j];
        }
    }
    for (usize j = 0; j < n; ++j)
        for (usize i = j + 1; i < m; ++i) r(i, j) = T(0);  // clear rounding residue below the diagonal
    return {std::move(q), std::move(r)};
}

/// Least-squares solution of min |A x - b|_2 (A is m x n, m >= n, full column rank).
template <Real T>
[[nodiscard]] VectorX<T> least_squares(const Matrix<T>& a, const VectorX<T>& b) {
    const usize m = a.rows(), n = a.cols();
    if (m < n) throw MathException("least_squares: need rows >= cols");
    if (b.size() != m) throw MathException("least_squares: right-hand side size mismatch");
    const auto [q, r] = qr_decompose(a);
    VectorX<T> qtb(m, T(0));
    for (usize j = 0; j < m; ++j)
        for (usize i = 0; i < m; ++i) qtb[j] += q(i, j) * b[i];
    T rmax = T(0);
    for (usize i = 0; i < n; ++i) rmax = std::max(rmax, std::fabs(r(i, i)));
    VectorX<T> x(n);
    for (usize i = n; i-- > 0;) {
        if (std::fabs(r(i, i)) <= rmax * T(m) * std::numeric_limits<T>::epsilon())
            throw MathException("least_squares: matrix is rank deficient");
        T s = qtb[i];
        for (usize j = i + 1; j < n; ++j) s -= r(i, j) * x[j];
        x[i] = s / r(i, i);
    }
    return x;
}

// ============================================================================
// Cholesky: A = L * L^T for symmetric positive definite A
// ============================================================================
template <Real T>
[[nodiscard]] Matrix<T> cholesky(const Matrix<T>& a) {
    if (!a.square()) throw MathException("Cholesky: matrix must be square");
    const usize n = a.rows();
    Matrix<T> l(n, n);
    for (usize i = 0; i < n; ++i) {
        for (usize j = 0; j <= i; ++j) {
            T s = a(i, j);
            for (usize k = 0; k < j; ++k) s -= l(i, k) * l(j, k);
            if (i == j) {
                if (!(s > T(0))) throw MathException("Cholesky: matrix is not positive definite");
                l(i, i) = std::sqrt(s);
            } else {
                l(i, j) = s / l(j, j);
            }
        }
    }
    return l;
}

/// Solves A x = b given the Cholesky factor L of A.
template <Real T>
[[nodiscard]] VectorX<T> cholesky_solve(const Matrix<T>& l, const VectorX<T>& b) {
    const usize n = l.rows();
    if (b.size() != n) throw MathException("cholesky_solve: size mismatch");
    VectorX<T> y(n);
    for (usize i = 0; i < n; ++i) {
        T s = b[i];
        for (usize k = 0; k < i; ++k) s -= l(i, k) * y[k];
        y[i] = s / l(i, i);
    }
    VectorX<T> x(n);
    for (usize i = n; i-- > 0;) {
        T s = y[i];
        for (usize k = i + 1; k < n; ++k) s -= l(k, i) * x[k];
        x[i] = s / l(i, i);
    }
    return x;
}

// ============================================================================
// symmetric eigen decomposition (cyclic Jacobi): A = V * diag(values) * V^T
// ============================================================================
template <Real T>
struct EigenResult {
    VectorX<T> values;     ///< sorted in descending order
    Matrix<T> vectors;     ///< column i is the unit eigenvector of values[i]
};

template <Real T>
[[nodiscard]] EigenResult<T> eigen_symmetric(const Matrix<T>& a_in, usize max_sweeps = 100) {
    if (!a_in.square()) throw MathException("eigen_symmetric: matrix must be square");
    const usize n = a_in.rows();
    for (usize i = 0; i < n; ++i)
        for (usize j = i + 1; j < n; ++j)
            if (!approx_equal(a_in(i, j), a_in(j, i), T(1e-6), T(1e-9)))
                throw MathException("eigen_symmetric: matrix is not symmetric");
    Matrix<T> a = a_in;
    Matrix<T> v = Matrix<T>::identity(n);
    bool converged = n < 2;
    for (usize sweep = 0; sweep < max_sweeps && !converged; ++sweep) {
        T off = T(0), diag = T(0);
        for (usize i = 0; i < n; ++i) {
            diag += a(i, i) * a(i, i);
            for (usize j = i + 1; j < n; ++j) off += a(i, j) * a(i, j);
        }
        if (off <= std::numeric_limits<T>::epsilon() * std::numeric_limits<T>::epsilon() * (diag + off) || off == T(0)) {
            converged = true;
            break;
        }
        for (usize p = 0; p < n; ++p)
            for (usize q = p + 1; q < n; ++q) {
                if (a(p, q) == T(0)) continue;
                const T theta = (a(q, q) - a(p, p)) / (T(2) * a(p, q));
                const T t = (theta >= T(0) ? T(1) : T(-1)) / (std::fabs(theta) + std::sqrt(theta * theta + T(1)));
                const T c = T(1) / std::sqrt(t * t + T(1));
                const T s = t * c;
                for (usize k = 0; k < n; ++k) {  // A = A J (columns)
                    const T akp = a(k, p), akq = a(k, q);
                    a(k, p) = c * akp - s * akq;
                    a(k, q) = s * akp + c * akq;
                }
                for (usize k = 0; k < n; ++k) {  // A = J^T A (rows)
                    const T apk = a(p, k), aqk = a(q, k);
                    a(p, k) = c * apk - s * aqk;
                    a(q, k) = s * apk + c * aqk;
                }
                for (usize k = 0; k < n; ++k) {
                    const T vkp = v(k, p), vkq = v(k, q);
                    v(k, p) = c * vkp - s * vkq;
                    v(k, q) = s * vkp + c * vkq;
                }
            }
    }
    if (!converged) {
        T off = T(0), tot = T(0);
        for (usize i = 0; i < n; ++i)
            for (usize j = 0; j < n; ++j) { tot += a(i, j) * a(i, j); if (i != j) off += a(i, j) * a(i, j); }
        if (off > T(1e-10) * tot) throw MathException("eigen_symmetric: Jacobi iteration did not converge");
    }
    std::vector<usize> order(n);
    std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize x, usize y) { return a(x, x) > a(y, y); });
    EigenResult<T> res{VectorX<T>(n), Matrix<T>(n, n)};
    for (usize i = 0; i < n; ++i) {
        res.values[i] = a(order[i], order[i]);
        for (usize k = 0; k < n; ++k) res.vectors(k, i) = v(k, order[i]);
    }
    return res;
}

// ============================================================================
// singular value decomposition (one-sided Jacobi): A = U * diag(S) * V^T
// thin form: U is m x k, S has k entries, V is n x k, k = min(m, n)
// ============================================================================
template <Real T>
struct SvdResult {
    Matrix<T> u;
    VectorX<T> s;   ///< descending, non-negative
    Matrix<T> v;
};

template <Real T>
[[nodiscard]] SvdResult<T> svd(const Matrix<T>& a, usize max_sweeps = 60) {
    if (a.empty()) throw MathException("SVD: empty matrix");
    if (a.rows() < a.cols()) {  // work on the transpose and swap the factors
        SvdResult<T> t = svd(a.transposed(), max_sweeps);
        return {std::move(t.v), std::move(t.s), std::move(t.u)};
    }
    const usize m = a.rows(), n = a.cols();
    Matrix<T> u = a;
    Matrix<T> v = Matrix<T>::identity(n);
    const T eps = std::numeric_limits<T>::epsilon();
    bool converged = false;
    for (usize sweep = 0; sweep < max_sweeps && !converged; ++sweep) {
        converged = true;
        for (usize i = 0; i + 1 < n; ++i)
            for (usize j = i + 1; j < n; ++j) {
                T alpha = T(0), beta = T(0), gamma = T(0);
                for (usize k = 0; k < m; ++k) {
                    alpha += u(k, i) * u(k, i);
                    beta += u(k, j) * u(k, j);
                    gamma += u(k, i) * u(k, j);
                }
                if (gamma == T(0) || std::fabs(gamma) <= eps * std::sqrt(alpha * beta)) continue;
                converged = false;
                const T zeta = (beta - alpha) / (T(2) * gamma);
                const T t = (zeta >= T(0) ? T(1) : T(-1)) / (std::fabs(zeta) + std::sqrt(T(1) + zeta * zeta));
                const T c = T(1) / std::sqrt(T(1) + t * t);
                const T s = c * t;
                for (usize k = 0; k < m; ++k) {
                    const T x = u(k, i), y = u(k, j);
                    u(k, i) = c * x - s * y;
                    u(k, j) = s * x + c * y;
                }
                for (usize k = 0; k < n; ++k) {
                    const T x = v(k, i), y = v(k, j);
                    v(k, i) = c * x - s * y;
                    v(k, j) = s * x + c * y;
                }
            }
    }
    if (!converged) throw MathException("SVD: Jacobi iteration did not converge");
    VectorX<T> sv(n);
    for (usize j = 0; j < n; ++j) {
        T s = T(0);
        for (usize k = 0; k < m; ++k) s += u(k, j) * u(k, j);
        sv[j] = std::sqrt(s);
    }
    std::vector<usize> order(n);
    std::iota(order.begin(), order.end(), usize{0});
    std::sort(order.begin(), order.end(), [&](usize x, usize y) { return sv[x] > sv[y]; });
    SvdResult<T> res{Matrix<T>(m, n), VectorX<T>(n), Matrix<T>(n, n)};
    for (usize jj = 0; jj < n; ++jj) {
        const usize j = order[jj];
        res.s[jj] = sv[j];
        for (usize k = 0; k < m; ++k) res.u(k, jj) = sv[j] > T(0) ? u(k, j) / sv[j] : T(0);
        for (usize k = 0; k < n; ++k) res.v(k, jj) = v(k, j);
    }
    return res;
}

/// Numerical rank: singular values above max(m,n) * eps * s_max.
template <Real T>
[[nodiscard]] usize rank(const Matrix<T>& a) {
    const auto r = svd(a);
    const T tol = static_cast<T>(std::max(a.rows(), a.cols())) * std::numeric_limits<T>::epsilon() * r.s.front();
    return static_cast<usize>(std::count_if(r.s.begin(), r.s.end(), [&](T x) { return x > tol; }));
}

/// 2-norm condition number (infinity for singular matrices).
template <Real T>
[[nodiscard]] T condition_number(const Matrix<T>& a) {
    const auto r = svd(a);
    return r.s.back() == T(0) ? infinity_v<T> : r.s.front() / r.s.back();
}

/// Moore-Penrose pseudo-inverse via SVD.
template <Real T>
[[nodiscard]] Matrix<T> pseudo_inverse(const Matrix<T>& a) {
    const auto r = svd(a);
    const T tol = static_cast<T>(std::max(a.rows(), a.cols())) * std::numeric_limits<T>::epsilon() * r.s.front();
    Matrix<T> p(a.cols(), a.rows());
    for (usize k = 0; k < r.s.size(); ++k) {
        if (!(r.s[k] > tol)) continue;
        for (usize i = 0; i < a.cols(); ++i)
            for (usize j = 0; j < a.rows(); ++j) p(i, j) += r.v(i, k) * r.u(j, k) / r.s[k];
    }
    return p;
}

// ============================================================================
// convenience wrappers
// ============================================================================

/// Solves A x = b (square A) by LU with partial pivoting. Throws when A is singular.
template <Real T>
[[nodiscard]] VectorX<T> solve(const Matrix<T>& a, const VectorX<T>& b) { return LuDecomposition<T>(a).solve(b); }

template <Real T> [[nodiscard]] T determinant(const Matrix<T>& a) { return LuDecomposition<T>(a).determinant(); }
template <Real T> [[nodiscard]] Matrix<T> inverse(const Matrix<T>& a) { return LuDecomposition<T>(a).inverse(); }

/// Residual norm |A x - b|_2: a backward-error check for a computed solution.
template <Real T>
[[nodiscard]] T residual_norm(const Matrix<T>& a, const VectorX<T>& x, const VectorX<T>& b) {
    VectorX<T> r = a * x;
    if (r.size() != b.size()) throw MathException("residual_norm: size mismatch");
    for (usize i = 0; i < r.size(); ++i) r[i] -= b[i];
    return norm(r);
}

/// One step of iterative refinement for A x = b; returns the improved x.
template <Real T>
[[nodiscard]] VectorX<T> refine(const LuDecomposition<T>& lu, const Matrix<T>& a, VectorX<T> x, const VectorX<T>& b) {
    VectorX<T> r = a * x;
    for (usize i = 0; i < r.size(); ++i) r[i] = b[i] - r[i];
    const VectorX<T> dx = lu.solve(r);
    for (usize i = 0; i < x.size(); ++i) x[i] += dx[i];
    return x;
}

}  // namespace flyg::math
