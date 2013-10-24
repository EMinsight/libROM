#include "matrix_utils.h"
#include <vector>

namespace CAROM {

// A class which embodies the incremental SVD algorithm.  The API is
// intentionally small.  One may increment the SVD, get the tolerance used
// to determine if an increment is new, and get the dimension of the system.
class incremental_svd
{
   public:
      // Constructor.
      incremental_svd(
         int dim,
         double epsilon,
         bool skip_redundant,
         int increments_per_time_interval);

      // Destructor.
      ~incremental_svd();

      // Increment the SVD with the new state, u_in, at the given time.
      void
      increment(
         double* u_in,
         double time);

      // Returns the tolerance used to determine if an increment is new.
      double
      getEpsilon() const
      {
         return d_epsilon;
      }

      // Returns the dimension of the system.
      int
      getDim() const
      {
         return d_dim;
      }

      // Returns the number of processors being run on.
      int
      getSize() const
      {
         return d_size;
      }

      // Returns the model parameters for the given time, d_U*d_L, as a C
      // array.
      double*
      getModel(
         double time) const;

      double
      getNormJ() const
      {
         return d_norm_j;
      }

   private:
      // Unimplemented default constructor.
      incremental_svd();

      // Unimplemented copy constructor.
      incremental_svd(const incremental_svd& other);

      // Unimplemented assignment operator.
      incremental_svd&
      operator = (const incremental_svd& rhs);

      // Constructs the first svd.
      void
      buildInitialSVD(
         double* u,
         double time);

      // Increments the svd given the state vector u.
      void
      buildIncrementalSVD(
         double* u);

      // Compute J, P, and the norm of J given u.
      void
      compute_J_P_normJ(
         double* u,
         double*& j,
         double*& P);

      // Use modified Gram-Schmidt orthogonalization to modify j then compute
      // its norm.
      void
      orthogonalizeJAndComputeNorm(
         double* j,
         double* P);

      // Construct the Q matrix which will be passed to svd.
      void
      constructQ(
         double*& Q,
         double* l,
         double norm_j);

      // Given a matrix, A, returns the 3 components of the singular value
      // decomposition.
      void
      svd(
         double* A,
         double*& U,
         double*& S,
         double*& V);

      // Add a redundant increment to the svd.
      void
      addRedundantIncrement(
         double* A,
         double* sigma);

      // Add a new, unique increment to the svd.
      void
      addNewIncrement(
         double* j,
         double* A,
         double* sigma);

      // Dimension of the system.
      int d_dim;

      // Number of increments stored.
      int d_num_increments;

      // Error tolerance.
      double d_epsilon;

      // If true, skip redundant increments.
      bool d_skip_redundant;

      // The number of increments to be collected for each time interval.
      int d_increments_per_time_interval;

      // For each time interval, the matrix U distributed across all
      // processors.  Each d_U is the part of the distributed matrix local to
      // the processor owning this object.
      std::vector<double*> d_U;

      // For each time interval, the matrix L.  L is not distributed and the
      // entire matrix exists on each processor.
      std::vector<double*> d_L;

      // For each time interval, the matrix S.  S is not distributed and the
      // entire matrix exists on each processor.
      std::vector<double*> d_S;

      // For each time interval, the model parameters distributed across all
      // processors.  Each d_model is the part of the distributed model
      // parameters local to the processor owning this object.
      std::vector<double*> d_model;

      // The number of time intervals gathered.
      int d_num_time_intervals;

      // The simulation time at which each time interval starts.
      std::vector<double> d_time_interval_start_times;

      // Rank of process this object lives on.
      int d_rank;

      // Total number of processors.
      int d_size;

      // Value of norm of j cached by increment.
      double d_norm_j;

      // MPI message tag.
      static const int COMMUNICATE_U;
};

}
