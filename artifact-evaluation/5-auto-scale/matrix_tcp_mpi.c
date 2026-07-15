#include <mpi.h>
#include <omp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void die(const char *what) {
    fprintf(stderr, "%s\n", what);
    MPI_Abort(MPI_COMM_WORLD, 1);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, ranks;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &ranks);
    int side = argc > 1 ? atoi(argv[1]) : 4000;
    if (side <= 0) die("matrix side must be positive");

    int base = side / ranks, extra = side % ranks;
    int local_rows = base + (rank < extra);
    int *counts = calloc((size_t)ranks, sizeof(*counts));
    int *offsets = calloc((size_t)ranks, sizeof(*offsets));
    for (int i = 0, off = 0; i < ranks; ++i) {
        counts[i] = (base + (i < extra)) * side;
        offsets[i] = off;
        off += counts[i];
    }

    size_t matrix_elems = (size_t)side * side;
    int32_t *a = rank == 0 ? malloc(matrix_elems * sizeof(*a)) : NULL;
    int32_t *b = malloc(matrix_elems * sizeof(*b));
    int32_t *c = rank == 0 ? malloc(matrix_elems * sizeof(*c)) : NULL;
    int32_t *local_a = malloc((size_t)local_rows * side * sizeof(*local_a));
    int32_t *local_c = malloc((size_t)local_rows * side * sizeof(*local_c));
    if (!b || !local_a || !local_c || (rank == 0 && (!a || !c))) die("matrix allocation failed");

    if (rank == 0) {
        for (size_t i = 0; i < matrix_elems; ++i) {
            a[i] = (int32_t)(i % 17);
            b[i] = (int32_t)(i % 13);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);
    double begin = MPI_Wtime();
    MPI_Bcast(b, (int)matrix_elems, MPI_INT32_T, 0, MPI_COMM_WORLD);
    MPI_Scatterv(a, counts, offsets, MPI_INT32_T, local_a, local_rows * side,
                 MPI_INT32_T, 0, MPI_COMM_WORLD);

#pragma omp parallel for schedule(static)
    for (int row = 0; row < local_rows; ++row) {
        for (int col = 0; col < side; ++col) {
            int32_t sum = 0;
            for (int k = 0; k < side; ++k)
                sum += local_a[(size_t)row * side + k] * b[(size_t)k * side + col];
            local_c[(size_t)row * side + col] = sum;
        }
    }
    MPI_Gatherv(local_c, local_rows * side, MPI_INT32_T, c, counts, offsets,
                MPI_INT32_T, 0, MPI_COMM_WORLD);
    double elapsed = MPI_Wtime() - begin;
    if (rank == 0) {
        uint64_t checksum = 0;
        for (size_t i = 0; i < matrix_elems; ++i) checksum += (uint32_t)c[i];
        printf("RESULT: N=%d CONFIG=TCP TIME=%llu CHECKSUM=%llu\n", ranks,
               (unsigned long long)(elapsed * 1000000.0),
               (unsigned long long)checksum);
    }
    free(a); free(b); free(c); free(local_a); free(local_c); free(counts); free(offsets);
    MPI_Finalize();
    return 0;
}
