/******************************************************************************
 *
 * Copyright (c) 2013-2024, Lawrence Livermore National Security, LLC
 * and other libROM project developers. See the top-level COPYRIGHT
 * file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 *
 *****************************************************************************/

// Description: A class that writes basis vectors to a file.

#include "BasisWriter.h"
#include "utils/HDFDatabase.h"
#include "Matrix.h"
#include "Vector.h"
#include "BasisGenerator.h"
#include "utils/Utilities.h"

#include "mpi.h"

namespace CAROM {

BasisWriter::BasisWriter(
    BasisGenerator* basis_generator,
    const std::string& base_file_name,
    Database::formats db_format) :
    d_basis_generator(basis_generator),
    full_file_name(""),
    snap_file_name(""),
    db_format_(db_format),
    d_database(NULL),
    d_snap_database(NULL)
{
    CAROM_ASSERT(basis_generator != 0);
    CAROM_ASSERT(!base_file_name.empty());

    int mpi_init;
    MPI_Initialized(&mpi_init);
    int rank;
    if (mpi_init) {
        MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    }
    else {
        rank = 0;
    }

    char tmp[100];
    sprintf(tmp, ".%06d", rank);
    full_file_name = base_file_name + tmp;

    char tmp2[100];
    sprintf(tmp2, "_snapshot.%06d", rank);
    snap_file_name = base_file_name + tmp2;

    // create and open snapshot/basis database
    CAROM_VERIFY(db_format_ == Database::HDF5);
    d_snap_database = new HDFDatabase();
    d_database = new HDFDatabase();
}

BasisWriter::~BasisWriter()
{
    delete d_database;
    delete d_snap_database;
}

void
BasisWriter::writeBasis(const std::string& kind)
{
    CAROM_ASSERT(kind == "basis" || kind == "snapshot");

    char tmp[100];

    if (kind == "basis") {
        d_database->create(full_file_name);

        const Matrix* basis = d_basis_generator->getSpatialBasis();
        int num_rows = basis->numRows();
        sprintf(tmp, "spatial_basis_num_rows");
        d_database->putInteger(tmp, num_rows);
        int num_cols = basis->numColumns();
        sprintf(tmp, "spatial_basis_num_cols");
        d_database->putInteger(tmp, num_cols);
        sprintf(tmp, "spatial_basis");
        d_database->putDoubleArray(tmp, &basis->item(0, 0), num_rows*num_cols);

        if(d_basis_generator->updateRightSV()) {
            const Matrix* tbasis = d_basis_generator->getTemporalBasis();
            num_rows = tbasis->numRows();
            sprintf(tmp, "temporal_basis_num_rows");
            d_database->putInteger(tmp, num_rows);
            num_cols = tbasis->numColumns();
            sprintf(tmp, "temporal_basis_num_cols");
            d_database->putInteger(tmp, num_cols);
            sprintf(tmp, "temporal_basis");
            d_database->putDoubleArray(tmp, &tbasis->item(0, 0), num_rows*num_cols);
        }

        const Vector* sv = d_basis_generator->getSingularValues();
        int sv_dim = sv->dim();
        sprintf(tmp, "singular_value_size");
        d_database->putInteger(tmp, sv_dim);
        sprintf(tmp, "singular_value");
        d_database->putDoubleArray(tmp, &sv->item(0), sv_dim);

        d_database->close();
    }

    if (kind == "snapshot") {
        d_snap_database->create(snap_file_name);

        const Matrix* snapshots = d_basis_generator->getSnapshotMatrix();
        int num_rows = snapshots->numRows(); // d_dim
        sprintf(tmp, "snapshot_matrix_num_rows");
        d_snap_database->putInteger(tmp, num_rows);
        int num_cols = snapshots->numColumns(); // d_num_samples
        sprintf(tmp, "snapshot_matrix_num_cols");
        d_snap_database->putInteger(tmp, num_cols);
        sprintf(tmp, "snapshot_matrix");
        d_snap_database->putDoubleArray(tmp, &snapshots->item(0,0), num_rows*num_cols);

        d_snap_database->close();
    }

}

}
