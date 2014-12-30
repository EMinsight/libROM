/******************************************************************************
 *
 * This file is part of the CAROM distribution.  For full copyright
 * information, see COPYRIGHT.
 *
 * Copyright:   (c) 2013-2014 Lawrence Livermore National Security, LLC
 * Description: The concrete implementation of the incremental SVD algorithm
 *              that is equivalent to but computationally more expensive than
 *              the "fast update" method.
 *
 *****************************************************************************/

#ifndef included_IncrementalSVDNaive_h
#define included_IncrementalSVDNaive_h

#include "IncrementalSVD.h"

namespace CAROM {

/**
 * A class which embodies the naive incremental SVD algorithm.
 */
class IncrementalSVDNaive : public IncrementalSVD
{
   public:
      /**
       * @brief Constructor.
       *
       * @pre dim > 0
       * @pre redundancy_tol > 0.0
       * @pre increments_per_time_interval > 0
       *
       * @param[in] dim The dimension of the system on this processor.
       * @param[in] redundancy_tol Tolerance to determine if an increment is
       *                           redundant or not.
       * @param[in] skip_redundant If true skip redundant increments.
       * @param[in] increments_per_time_interval The number of increments to be
       *                                         collected for each time
       *                                         interval.
       */
      IncrementalSVDNaive(
         int dim,
         double redundancy_tol,
         bool skip_redundant,
         int increments_per_time_interval);

      /**
       * @brief Destructor.
       */
      ~IncrementalSVDNaive();

      /**
       * @brief Increment the SVD with the new state, u_in, at the given time..
       *
       * @pre u_in != 0
       * @pre time >= 0.0
       *
       * @param[in] u_in The state at the specified time.
       * @param[in] time The simulation time for the state.
       */
      void
      increment(
         const double* u_in,
         double time);

      /**
       * @brief Returns the basis vectors for the current time interval as a
       * Matrix.
       *
       * @return The basis vectors for the current time interval.
       */
      const Matrix*
      getBasis();

   private:
      /**
       * @brief Unimplemented default constructor.
       */
      IncrementalSVDNaive();

      /**
       * @brief Unimplemented copy constructor.
       */
      IncrementalSVDNaive(
         const IncrementalSVDNaive& other);

      /**
       * @brief Unimplemented assignment operator.
       */
      IncrementalSVDNaive&
      operator = (
         const IncrementalSVDNaive& rhs);

      /**
       * @brief Constructs the first svd.
       *
       * @pre u != 0
       * @pre time >= 0.0
       *
       * @param[in] u The first state.
       * @param[in] time The simulation time for the first state.
       */
      void
      buildInitialSVD(
         const double* u,
         double time);

      /**
       * @brief Increments the svd given the state vector u.
       *
       * @pre u != 0
       *
       * @param[in] u The new state.
       */
      void
      buildIncrementalSVD(
         const double* u);

      /**
       * Add a redundant increment to the svd.
       *
       * @pre A != 0
       * @pre sigma != 0
       *
       * @param[in] A The left singular vectors.
       * @param[in] sigma The singular values.
       */
      void
      addRedundantIncrement(
         const Matrix* A,
         const Matrix* sigma);

      /**
       * @brief Add a new, unique increment to the svd.
       *
       * @pre j != 0
       * @pre A != 0
       * @pre sigma != 0
       *
       * @param[in] j The new column of d_U.
       * @param[in] A The left singular vectors.
       * @param[in] sigma The singular values.
       */
      void
      addNewIncrement(
         const Vector* j,
         const Matrix* A,
         Matrix* sigma);

      /**
       * @brief Computes and returns the orthogonality of d_U.
       *
       * @return The orthogonality of d_U.
       */
      double
      checkOrthogonality();

      /**
       * @brief Reorthogonalizes d_U.
       */
      void
      reOrthogonalize();

      /**
       * @brief The matrix U distributed across all processors.
       *
       * Each processor's d_U is the part of the distributed matrix local to
       * that processor.
       */
      Matrix* d_U;
};

}

#endif
