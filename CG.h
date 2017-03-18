#ifndef CG_H
#define CG_H

#include <cassert>
#include <chrono>
#include <memory>

#include "Matrix.h"
#include "Preconditioner.h"
#include "def.h"

/// @brief The base class implementing the conjugate gradients method.
///
/// It is used to solve the equation system Ax = k. A is a sparse matrix
/// stored either COO, CRS or ELLPACK format.
class CG {
public:
  /// Different vectors used to solve the equation system.
  enum Vector {
    /// LHS of the equation system.
    VectorK,
    /// Computed solution of the equation system.
    VectorX,
    /// Temporary vector for the search direction.
    VectorP,
    /// Temporary vector holding the result of the matrix vector multiplication.
    VectorQ,
    /// Temporary vector for the residual.
    VectorR,
    /// Temporary vector in use with the preconditioner.
    VectorZ,
  };

  /// Different formats used to store the sparse matrix.
  enum MatrixFormat {
    /// %Matrix is represented by CG#matrixCOO.
    MatrixFormatCOO,
    /// %Matrix is represented by CG#matrixCRS.
    MatrixFormatCRS,
    /// %Matrix is represented by CG#matrixELL.
    MatrixFormatELL,
  };

  /// Different preconditioners to use.
  enum Preconditioner {
    /// Use no preconditioner.
    PreconditionerNone,
    /// Use a Jacobi preconditioner.
    PreconditionerJacobi,
  };

private:
  int iteration;
  int maxIterations = 1000;

  floatType residual;
  floatType tolerance = 1e-9;

  /// Struct holding timing information for IO, converting, the total solve time
  /// and for each kernel.
  struct Timing {
    using clock = std::chrono::steady_clock;
    using duration = std::chrono::duration<double>;

    duration io;
    duration converting;
    duration solve;
    duration matvec;
    duration axpy;
    duration xpay;
    duration vectorDot;
    duration preconditioner;
  };
  Timing timing;

  using time_point = Timing::clock::time_point;
  time_point now() const { return Timing::clock::now(); }

  void matvec(Vector in, Vector out) {
    time_point start = now();
    matvecKernel(in, out);
    timing.matvec += now() - start;
  }

  void axpy(floatType a, Vector x, Vector y) {
    time_point start = now();
    axpyKernel(a, x, y);
    timing.axpy += now() - start;
  }

  void xpay(Vector x, floatType a, Vector y) {
    time_point start = now();
    xpayKernel(x, a, y);
    timing.xpay += now() - start;
  }

  floatType vectorDot(Vector a, Vector b) {
    time_point start = now();
    floatType res = vectorDotKernel(a, b);
    timing.vectorDot += now() - start;

    return res;
  }

  void applyPreconditioner(Vector x, Vector y) {
    time_point start = now();
    applyPreconditionerKernel(x, y);
    timing.preconditioner += now() - start;
  }

protected:
  /// Dimension of the matrix.
  int N;
  /// Nonzeros in the matrix.
  int nz;

  /// Format to store the matrix.
  MatrixFormat matrixFormat;
  /// Matrix in cooridinate format.
  std::unique_ptr<MatrixCOO> matrixCOO;
  /// Matrix in CRS format.
  std::unique_ptr<MatrixCRS> matrixCRS;
  /// Matrix in ELLPACK format.
  std::unique_ptr<MatrixELL> matrixELL;

  /// The preconditioner to use.
  Preconditioner preconditioner = PreconditionerNone;
  /// Jacobi preconditioner.
  std::unique_ptr<Jacobi> jacobi;

  /// #VectorK
  std::unique_ptr<floatType[]> k;
  /// #VectorX
  std::unique_ptr<floatType[]> x;

  /// Construct a new object with a \a defaultMatrixFormat to store the matrix.
  CG(MatrixFormat defaultMatrixFormat) : matrixFormat(defaultMatrixFormat) {}
  /// Construct a new object with a \a defaultMatrixFormat to store tha matrix
  /// and a \a defaultPreconditioner to use.
  CG(MatrixFormat defaultMatrixFormat, Preconditioner defaultPreconditioner)
      : matrixFormat(defaultMatrixFormat),
        preconditioner(defaultPreconditioner) {}

  /// @return \a true if this implementation supports \a format to store the matrix.
  virtual bool supportsMatrixFormat(MatrixFormat format) = 0;
  /// @return \a true if this implementation supports the \a preconditioner.
  virtual bool supportsPreconditioner(Preconditioner preconditioner) {
    return false;
  }

  /// Convert to MatrixCRS.
  virtual void convertToMatrixCRS() {
    matrixCRS.reset(new MatrixCRS(*matrixCOO));
  }
  /// Convert to MatrixELL.
  virtual void convertToMatrixELL() {
    matrixELL.reset(new MatrixELL(*matrixCOO));
  }

  /// Initialize the Jacobi preconditioner.
  virtual void initJacobi() { jacobi.reset(new Jacobi(*matrixCOO)); }

  /// Allocate #k.
  virtual void allocateK() { k.reset(new floatType[N]); }
  /// Allocate #x.
  virtual void allocateX() { x.reset(new floatType[N]); }

  /// Copy vector \a _src to \a _dst.
  virtual void cpy(Vector _dst, Vector _src) = 0;
  /// \a _y = A * \a _x.
  virtual void matvecKernel(Vector _x, Vector _y) = 0;
  /// \a _y = \a a * \a _x + \a _y.
  virtual void axpyKernel(floatType a, Vector _x, Vector _y) = 0;
  /// \a _y = \a _y + \a a * \a _y.
  virtual void xpayKernel(Vector _x, floatType a, Vector _y) = 0;
  /// @return vector dot product <\a _a, \a _b>
  virtual floatType vectorDotKernel(Vector _a, Vector _b) = 0;

  /// \a _y = B * \a _x
  virtual void applyPreconditionerKernel(Vector _x, Vector _y) {
    assert(0 && "Preconditioner not implemented!");
  }

  /// Print \a label (padded to a constant number of characters) and \a value.
  static void printPadded(const char *label, const std::string &value);

public:
  /// Parse and validate environment variables.
  virtual void parseEnvironment();
  /// Init data by reading matrix from \a matrixFile.
  virtual void init(const char *matrixFile);

  /// Solve sparse equation system.
  void solve();

  /// Print summary after system has been solved.
  virtual void printSummary();

  /// @return new instance of a CG implementation.
  static CG *getInstance();
};

#endif
