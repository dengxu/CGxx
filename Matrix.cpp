#include <cassert>
#include <cstdio>
#include <cstring>
#include <iostream>

#include "Matrix.h"
#include "WorkDistribution.h"

extern "C" {
#include "mmio.h"
};

MatrixCOO::MatrixCOO(const char *file) {
  FILE *fp = fopen(file, "r");
  if (fp == NULL) {
    std::cerr << "ERROR: Can't open file!" << std::endl;
    std::exit(1);
  }

  MM_typecode matcode;
  if (mm_read_banner(fp, &matcode) != 0) {
    std::cerr << "ERROR: Could not process Matrix Market banner!" << std::endl;
    std::exit(1);
  }

  // Check properties.
  if (!mm_is_sparse(matcode) || !mm_is_real(matcode)) {
    std::cerr << "ERROR: Only supporting real matrixes in coordinate format!";
    std::cerr << " (type: " << mm_typecode_to_str(matcode) << ")" << std::endl;
    std::exit(1);
  }

  int M;
  if (mm_read_mtx_crd_size(fp, &M, &N, &nz) != 0) {
    std::cerr << "ERROR: Could not read matrix size!" << std::endl;
    std::exit(1);
  }

  if (N != M) {
    std::cerr << "ERROR: Need a quadratic matrix!" << std::endl;
    std::exit(1);
  }

  bool symmetric = mm_is_symmetric(matcode);
  if (symmetric) {
    // Store upper and lower triangular!
    nz = 2 * nz - N;
  }

  // Allocate memory. No implementation will need to "optimize" this because
  // MatrixCOO is not really meant to be used in "real" computations.
  I.reset(new int[nz]);
  J.reset(new int[nz]);
  V.reset(new floatType[nz]);
  nzPerRow.reset(new int[N]);
  std::memset(nzPerRow.get(), 0, sizeof(int) * N);

  // Read matrix.
  for (int i = 0; i < nz; i++) {
    fscanf(fp, "%d %d %lg\n", &I[i], &J[i], &V[i]);

    // Adjust from 1-based to 0-based.
    I[i]--;
    J[i]--;

    // Count nz for each row.
    nzPerRow[I[i]]++;

    // If not on the main diagonal, we have to duplicate the entry.
    if (symmetric && I[i] != J[i]) {
      i++;
      I[i] = J[i - 1];
      J[i] = I[i - 1];
      V[i] = V[i - 1];

      // Count nz for each row. I[i] is now J[i -1]!
      nzPerRow[I[i]]++;
    }
  }

  fclose(fp);
}

int MatrixCOO::getMaxNz(int from, int to) const {
  int maxNz = 0;
  for (int i = from; i < to; i++) {
    if (nzPerRow[i] > maxNz) {
      maxNz = nzPerRow[i];
    }
  }
  return maxNz;
}

template <> DataMatrix<MatrixDataCRS>::DataMatrix(const MatrixCOO &coo) {
  N = coo.N;
  nz = coo.nz;

  // Temporary memory to store current offset in index / value per row.
  std::unique_ptr<int[]> offsets(new int[N]);

  // Construct ptr and initial values for offsets.
  allocatePtr(N);
  ptr[0] = 0;
  for (int i = 1; i <= N; i++) {
    // Copy ptr[i - 1] as initial value for offsets[i - 1].
    offsets[i - 1] = ptr[i - 1];

    ptr[i] = ptr[i - 1] + coo.nzPerRow[i - 1];
  }

  // Construct index and value.
  allocateIndexAndValue(nz);
  for (int i = 0; i < nz; i++) {
    int row = coo.I[i];
    index[offsets[row]] = coo.J[i];
    value[offsets[row]] = coo.V[i];
    offsets[row]++;
  }
}

template <>
SplitMatrix<MatrixDataCRS>::SplitMatrix(const MatrixCOO &coo,
                                        const WorkDistribution &wd) {
  N = coo.N;
  nz = coo.nz;
  allocateData(wd.numberOfChunks);

  // Temporary memory to store current offset in index / value per row.
  std::unique_ptr<int[]> offsets(new int[N]);

  // Construct ptr and initial values for offsets for each chunk.
  for (int c = 0; c < wd.numberOfChunks; c++) {
    int offset = wd.offsets[c];
    int length = wd.lengths[c];

    data[c].allocatePtr(length);
    data[c].ptr[0] = 0;
    for (int i = 1; i <= length; i++) {
      offsets[offset + i - 1] = data[c].ptr[i - 1];

      data[c].ptr[i] = data[c].ptr[i - 1] + coo.nzPerRow[offset + i - 1];
    }
  }

  // Allocate index and value for each chunk.
  for (int c = 0; c < wd.numberOfChunks; c++) {
    int length = wd.lengths[c];
    int values = data[c].ptr[length];
    data[c].allocateIndexAndValue(values);
  }

  // Construct index and value for all chunks.
  for (int i = 0; i < nz; i++) {
    int row = coo.I[i];
    int chunk = wd.findChunk(row);

    data[chunk].index[offsets[row]] = coo.J[i];
    data[chunk].value[offsets[row]] = coo.V[i];
    offsets[row]++;
  }
}

template <> DataMatrix<MatrixDataELL>::DataMatrix(const MatrixCOO &coo) {
  N = coo.N;
  nz = coo.nz;
  elements = N * coo.getMaxNz();

  // Copy over already collected nonzeros per row.
  allocateLength(N);
  std::memcpy(length, coo.nzPerRow.get(), sizeof(int) * N);

  // Temporary memory to store current offset in index / value per row.
  std::unique_ptr<int[]> offsets(new int[N]);
  std::memset(offsets.get(), 0, sizeof(int) * N);

  // Construct column and data.
  allocateIndexAndData();
  for (int i = 0; i < nz; i++) {
    int row = coo.I[i];
    int k = offsets[row] * N + row;
    index[k] = coo.J[i];
    data[k] = coo.V[i];
    offsets[row]++;
  }
}

template <>
SplitMatrix<MatrixDataELL>::SplitMatrix(const MatrixCOO &coo,
                                        const WorkDistribution &wd) {
  N = coo.N;
  nz = coo.nz;
  allocateData(wd.numberOfChunks);

  // Allocate length for each chunk.
  for (int c = 0; c < wd.numberOfChunks; c++) {
    int offset = wd.offsets[c];
    int length = wd.lengths[c];
    int maxNz = coo.getMaxNz(offset, offset + length);

    // Copy over already collected nonzeros per row.
    data[c].allocateLength(length);
    std::memcpy(data[c].length, coo.nzPerRow.get() + offset,
                sizeof(int) * length);

    data[c].elements = maxNz * length;
    data[c].allocateIndexAndData();
  }

  // Temporary memory to store current offset in index / value per row.
  std::unique_ptr<int[]> offsets(new int[N]);
  std::memset(offsets.get(), 0, sizeof(int) * N);

  // Construct column and data for all chunks.
  for (int i = 0; i < nz; i++) {
    int row = coo.I[i];
    int chunk = wd.findChunk(row);

    int k = offsets[row] * wd.lengths[chunk] + row - wd.offsets[chunk];
    data[chunk].index[k] = coo.J[i];
    data[chunk].data[k] = coo.V[i];
    offsets[row]++;
  }
}
