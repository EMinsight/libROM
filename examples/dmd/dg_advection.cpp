/******************************************************************************
 *
 * Copyright (c) 2013-2024, Lawrence Livermore National Security, LLC
 * and other libROM project developers. See the top-level COPYRIGHT
 * file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 *
 *****************************************************************************/

//                       libROM MFEM Example: DG Advection (adapted from ex9p.cpp)
//
// Compile with: make dg_advection
//
// =================================================================================
//
// Sample runs and results for DMD:
//
// Command 1:
//   mpirun -np 8 dg_advection -p 0 -dt 0.01 -tf 2 -visit
//
// Output 1:
//   Relative error of DMD solution (u) at t_final: 2 is 0.00031683336
//
// Command 2:
//   mpirun -np 8 dg_advection -p 3 -rp 1 -dt 0.005 -tf 4 -visit
//
// Output 2:
//   Relative error of DMD solution (u) at t_final: 4 is 0.00019053762
//
// =================================================================================
//
// Description:  This example code solves the time-dependent advection equation
//               du/dt + v.grad(u) = 0, where v is a given fluid velocity, and
//               u0(x)=u(0,x) is a given initial condition.
//
//               The example demonstrates the use of Discontinuous Galerkin (DG)
//               bilinear forms in MFEM (face integrators), the use of implicit
//               and explicit ODE time integrators, the definition of periodic
//               boundary conditions through periodic meshes, as well as the use
//               of GLVis for persistent visualization of a time-evolving
//               solution. Saving of time-dependent data files for visualization
//               with VisIt (visit.llnl.gov) and ParaView (paraview.org), as
//               well as the optional saving with ADIOS2 (adios2.readthedocs.io)
//               are also illustrated.

#include "mfem.hpp"
#include "algo/DMD.h"
#include "linalg/Vector.h"
#include <cmath>
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

// Choice for the problem setup. The fluid velocity, initial condition and
// inflow boundary condition are chosen based on this parameter.
int problem;

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v);

// Initial condition
double u0_function(const Vector &x);

// Inflow boundary condition
double inflow_function(const Vector &x);

// Mesh bounding box
Vector bb_min, bb_max;

// Type of preconditioner for implicit time integrator
enum class PrecType : int
{
    ILU = 0,
    AIR = 1
};

#if MFEM_HYPRE_VERSION >= 21800
// Algebraic multigrid preconditioner for advective problems based on
// approximate ideal restriction (AIR). Most effective when matrix is
// first scaled by DG block inverse, and AIR applied to scaled matrix.
// See https://doi.org/10.1137/17M1144350.
class AIR_prec : public Solver
{
private:
    const HypreParMatrix *A;
    // Copy of A scaled by block-diagonal inverse
    HypreParMatrix A_s;

    HypreBoomerAMG *AIR_solver;
    int blocksize;

public:
    AIR_prec(int blocksize_) : AIR_solver(NULL), blocksize(blocksize_) { }

    void SetOperator(const Operator &op)
    {
        width = op.Width();
        height = op.Height();

        A = dynamic_cast<const HypreParMatrix *>(&op);
        MFEM_VERIFY(A != NULL, "AIR_prec requires a HypreParMatrix.")

        // Scale A by block-diagonal inverse
        BlockInverseScale(A, &A_s, NULL, NULL, blocksize,
                          BlockInverseScaleJob::MATRIX_ONLY);
        delete AIR_solver;
        AIR_solver = new HypreBoomerAMG(A_s);
        AIR_solver->SetAdvectiveOptions(1, "", "FA");
        AIR_solver->SetPrintLevel(0);
        AIR_solver->SetMaxLevels(50);
    }

    virtual void Mult(const Vector &x, Vector &y) const
    {
        // Scale the rhs by block inverse and solve system
        HypreParVector z_s;
        BlockInverseScale(A, NULL, &x, &z_s, blocksize,
                          BlockInverseScaleJob::RHS_ONLY);
        AIR_solver->Mult(z_s, y);
    }

    ~AIR_prec()
    {
        delete AIR_solver;
    }
};
#endif

class DG_Solver : public Solver
{
private:
    HypreParMatrix &M, &K;
    SparseMatrix M_diag;
    HypreParMatrix *A;
    GMRESSolver linear_solver;
    Solver *prec;
    double dt;
public:
    DG_Solver(HypreParMatrix &M_, HypreParMatrix &K_, const FiniteElementSpace &fes,
              PrecType prec_type)
        : M(M_),
          K(K_),
          A(NULL),
          linear_solver(M.GetComm()),
          dt(-1.0)
    {
        int block_size = fes.GetFE(0)->GetDof();
        if (prec_type == PrecType::ILU)
        {
            prec = new BlockILU(block_size,
                                BlockILU::Reordering::MINIMUM_DISCARDED_FILL);
        }
        else if (prec_type == PrecType::AIR)
        {
#if MFEM_HYPRE_VERSION >= 21800
            prec = new AIR_prec(block_size);
#else
            MFEM_ABORT("Must have MFEM_HYPRE_VERSION >= 21800 to use AIR.\n");
#endif
        }
        linear_solver.iterative_mode = false;
        linear_solver.SetRelTol(1e-9);
        linear_solver.SetAbsTol(0.0);
        linear_solver.SetMaxIter(100);
        linear_solver.SetPrintLevel(0);
        linear_solver.SetPreconditioner(*prec);

        M.GetDiag(M_diag);
    }

    void SetTimeStep(double dt_)
    {
        if (dt_ != dt)
        {
            dt = dt_;
            // Form operator A = M - dt*K
            delete A;
            A = Add(-dt, K, 0.0, K);
            SparseMatrix A_diag;
            A->GetDiag(A_diag);
            A_diag.Add(1.0, M_diag);
            // this will also call SetOperator on the preconditioner
            linear_solver.SetOperator(*A);
        }
    }

    void SetOperator(const Operator &op)
    {
        linear_solver.SetOperator(op);
    }

    virtual void Mult(const Vector &x, Vector &y) const
    {
        linear_solver.Mult(x, y);
    }

    ~DG_Solver()
    {
        delete prec;
        delete A;
    }
};

/** A time-dependent operator for the right-hand side of the ODE. The DG weak
    form of du/dt = -v.grad(u) is M du/dt = K u + b, where M and K are the mass
    and advection matrices, and b describes the flow on the boundary. This can
    be written as a general ODE, du/dt = M^{-1} (K u + b), and this class is
    used to evaluate the right-hand side. */
class FE_Evolution : public TimeDependentOperator
{
private:
    OperatorHandle M, K;
    const Vector &b;
    Solver *M_prec;
    CGSolver M_solver;
    DG_Solver *dg_solver;

    mutable Vector z;

public:
    FE_Evolution(ParBilinearForm &M_, ParBilinearForm &K_, const Vector &b_,
                 PrecType prec_type);

    virtual void Mult(const Vector &x, Vector &y) const;
    virtual void ImplicitSolve(const double dt, const Vector &x, Vector &k);

    virtual ~FE_Evolution();
};

int main(int argc, char *argv[])
{
    // 1. Initialize MPI.
    Mpi::Init();
    const int num_procs = Mpi::WorldSize();
    const int myid = Mpi::WorldRank();

    // 2. Parse command-line options.
    problem = 0;
    const char *mesh_file = "../data/periodic-hexagon.mesh";
    int ser_ref_levels = 2;
    int par_ref_levels = 0;
    int order = 3;
    bool pa = false;
    bool ea = false;
    bool fa = false;
    const char *device_config = "cpu";
    int ode_solver_type = 4;
    double t_final = 10.0;
    double dt = 0.01;
    double ef = 0.9999;
    int rdim = -1;
    bool visualization = true;
    bool visit = false;
    bool paraview = false;
    bool adios2 = false;
    bool binary = false;
    int vis_steps = 5;
#if MFEM_HYPRE_VERSION >= 21800
    PrecType prec_type = PrecType::AIR;
#else
    PrecType prec_type = PrecType::ILU;
#endif
    int precision = 8;
    cout.precision(precision);

    OptionsParser args(argc, argv);
    args.AddOption(&mesh_file, "-m", "--mesh",
                   "Mesh file to use.");
    args.AddOption(&problem, "-p", "--problem",
                   "Problem setup to use. See options in velocity_function().");
    args.AddOption(&ser_ref_levels, "-rs", "--refine-serial",
                   "Number of times to refine the mesh uniformly in serial.");
    args.AddOption(&par_ref_levels, "-rp", "--refine-parallel",
                   "Number of times to refine the mesh uniformly in parallel.");
    args.AddOption(&order, "-o", "--order",
                   "Order (degree) of the finite elements.");
    args.AddOption(&pa, "-pa", "--partial-assembly", "-no-pa",
                   "--no-partial-assembly", "Enable Partial Assembly.");
    args.AddOption(&ea, "-ea", "--element-assembly", "-no-ea",
                   "--no-element-assembly", "Enable Element Assembly.");
    args.AddOption(&fa, "-fa", "--full-assembly", "-no-fa",
                   "--no-full-assembly", "Enable Full Assembly.");
    args.AddOption(&device_config, "-d", "--device",
                   "Device configuration string, see Device::Configure().");
    args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                   "ODE solver: 1 - Forward Euler,\n\t"
                   "            2 - RK2 SSP, 3 - RK3 SSP, 4 - RK4, 6 - RK6,\n\t"
                   "            11 - Backward Euler,\n\t"
                   "            12 - SDIRK23 (L-stable), 13 - SDIRK33,\n\t"
                   "            22 - Implicit Midpoint Method,\n\t"
                   "            23 - SDIRK23 (A-stable), 24 - SDIRK34");
    args.AddOption(&t_final, "-tf", "--t-final",
                   "Final time; start time is 0.");
    args.AddOption(&dt, "-dt", "--time-step",
                   "Time step.");
    args.AddOption((int *)&prec_type, "-pt", "--prec-type", "Preconditioner for "
                   "implicit solves. 0 for ILU, 1 for pAIR-AMG.");
    args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                   "--no-visualization",
                   "Enable or disable GLVis visualization.");
    args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                   "--no-visit-datafiles",
                   "Save data files for VisIt (visit.llnl.gov) visualization.");
    args.AddOption(&paraview, "-paraview", "--paraview-datafiles", "-no-paraview",
                   "--no-paraview-datafiles",
                   "Save data files for ParaView (paraview.org) visualization.");
    args.AddOption(&adios2, "-adios2", "--adios2-streams", "-no-adios2",
                   "--no-adios2-streams",
                   "Save data using adios2 streams.");
    args.AddOption(&binary, "-binary", "--binary-datafiles", "-ascii",
                   "--ascii-datafiles",
                   "Use binary (Sidre) or ascii format for VisIt data files.");
    args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                   "Visualize every n-th timestep.");
    args.AddOption(&ef, "-ef", "--energy_fraction",
                   "Energy fraction for DMD.");
    args.AddOption(&rdim, "-rdim", "--rdim",
                   "Reduced dimension for DMD.");
    args.Parse();
    if (!args.Good())
    {
        if (myid == 0)
        {
            args.PrintUsage(cout);
        }
        return 1;
    }
    if (myid == 0)
    {
        args.PrintOptions(cout);
    }

    Device device(device_config);
    if (myid == 0) {
        device.Print();
    }

    // 3. Read the serial mesh from the given mesh file on all processors. We can
    //    handle geometrically periodic meshes in this code.
    Mesh *mesh = new Mesh(mesh_file, 1, 1);
    int dim = mesh->Dimension();

    // 4. Define the ODE solver used for time integration. Several explicit
    //    Runge-Kutta methods are available.
    ODESolver *ode_solver = NULL;
    switch (ode_solver_type)
    {
    // Explicit methods
    case 1:
        ode_solver = new ForwardEulerSolver;
        break;
    case 2:
        ode_solver = new RK2Solver(1.0);
        break;
    case 3:
        ode_solver = new RK3SSPSolver;
        break;
    case 4:
        ode_solver = new RK4Solver;
        break;
    case 6:
        ode_solver = new RK6Solver;
        break;
    // Implicit (L-stable) methods
    case 11:
        ode_solver = new BackwardEulerSolver;
        break;
    case 12:
        ode_solver = new SDIRK23Solver(2);
        break;
    case 13:
        ode_solver = new SDIRK33Solver;
        break;
    // Implicit A-stable methods (not L-stable)
    case 22:
        ode_solver = new ImplicitMidpointSolver;
        break;
    case 23:
        ode_solver = new SDIRK23Solver;
        break;
    case 24:
        ode_solver = new SDIRK34Solver;
        break;
    default:
        if (myid == 0)
        {
            cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
        }
        delete mesh;
        return 3;
    }

    // 5. Refine the mesh in serial to increase the resolution. In this example
    //    we do 'ser_ref_levels' of uniform refinement, where 'ser_ref_levels' is
    //    a command-line parameter. If the mesh is of NURBS type, we convert it
    //    to a (piecewise-polynomial) high-order mesh.
    for (int lev = 0; lev < ser_ref_levels; lev++)
    {
        mesh->UniformRefinement();
    }
    if (mesh->NURBSext)
    {
        mesh->SetCurvature(max(order, 1));
    }
    mesh->GetBoundingBox(bb_min, bb_max, max(order, 1));

    // 6. Define the parallel mesh by a partitioning of the serial mesh. Refine
    //    this mesh further in parallel to increase the resolution. Once the
    //    parallel mesh is defined, the serial mesh can be deleted.
    ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
    delete mesh;
    for (int lev = 0; lev < par_ref_levels; lev++)
    {
        pmesh->UniformRefinement();
    }

    // 7. Define the parallel discontinuous DG finite element space on the
    //    parallel refined mesh of the given polynomial order.
    DG_FECollection fec(order, dim, BasisType::GaussLobatto);
    ParFiniteElementSpace *fes = new ParFiniteElementSpace(pmesh, &fec);

    HYPRE_BigInt global_vSize = fes->GlobalTrueVSize();
    if (myid == 0)
    {
        cout << "Number of unknowns: " << global_vSize << endl;
    }

    // 8. Set up and assemble the parallel bilinear and linear forms (and the
    //    parallel hypre matrices) corresponding to the DG discretization. The
    //    DGTraceIntegrator involves integrals over mesh interior faces.
    VectorFunctionCoefficient velocity(dim, velocity_function);
    FunctionCoefficient inflow(inflow_function);
    FunctionCoefficient u0(u0_function);

    ParBilinearForm *m = new ParBilinearForm(fes);
    ParBilinearForm *k = new ParBilinearForm(fes);
    if (pa)
    {
        m->SetAssemblyLevel(AssemblyLevel::PARTIAL);
        k->SetAssemblyLevel(AssemblyLevel::PARTIAL);
    }
    else if (ea)
    {
        m->SetAssemblyLevel(AssemblyLevel::ELEMENT);
        k->SetAssemblyLevel(AssemblyLevel::ELEMENT);
    }
    else if (fa)
    {
        m->SetAssemblyLevel(AssemblyLevel::FULL);
        k->SetAssemblyLevel(AssemblyLevel::FULL);
    }

    m->AddDomainIntegrator(new MassIntegrator);
    constexpr double alpha = -1.0;
    k->AddDomainIntegrator(new ConvectionIntegrator(velocity, alpha));
    k->AddInteriorFaceIntegrator(
        new NonconservativeDGTraceIntegrator(velocity, alpha));
    k->AddBdrFaceIntegrator(
        new NonconservativeDGTraceIntegrator(velocity, alpha));

    ParLinearForm *b = new ParLinearForm(fes);
    b->AddBdrFaceIntegrator(
        new BoundaryFlowIntegrator(inflow, velocity, alpha));

    int skip_zeros = 0;
    m->Assemble();
    k->Assemble(skip_zeros);
    b->Assemble();
    m->Finalize();
    k->Finalize(skip_zeros);

    HypreParVector *B = b->ParallelAssemble();

    // 9. Define the initial conditions, save the corresponding grid function to
    //    a file and (optionally) save data in the VisIt format and initialize
    //    GLVis visualization.
    ParGridFunction *u = new ParGridFunction(fes);
    u->ProjectCoefficient(u0);
    HypreParVector *U = u->GetTrueDofs();

    {
        ostringstream mesh_name, sol_name;
        mesh_name << "dg_advection-mesh." << setfill('0') << setw(6) << myid;
        sol_name << "dg_advection-init." << setfill('0') << setw(6) << myid;
        ofstream omesh(mesh_name.str().c_str());
        omesh.precision(precision);
        pmesh->Print(omesh);
        ofstream osol(sol_name.str().c_str());
        osol.precision(precision);
        u->Save(osol);
    }

    // Create data collection for solution output: either VisItDataCollection for
    // ascii data files, or SidreDataCollection for binary data files.
    DataCollection *dc = NULL;
    if (visit)
    {
        if (binary)
        {
#ifdef MFEM_USE_SIDRE
            dc = new SidreDataCollection("DG_Advection", pmesh);
#else
            MFEM_ABORT("Must build with MFEM_USE_SIDRE=YES for binary output.");
#endif
        }
        else
        {
            dc = new VisItDataCollection("DG_Advection", pmesh);
            dc->SetPrecision(precision);
            // To save the mesh using MFEM's parallel mesh format:
            // dc->SetFormat(DataCollection::PARALLEL_FORMAT);
        }
        dc->RegisterField("solution", u);
        dc->SetCycle(0);
        dc->SetTime(0.0);
        dc->Save();
    }

    ParaViewDataCollection *pd = NULL;
    if (paraview)
    {
        pd = new ParaViewDataCollection("DG_Advection", pmesh);
        pd->SetPrefixPath("ParaView");
        pd->RegisterField("solution", u);
        pd->SetLevelsOfDetail(order);
        pd->SetDataFormat(VTKFormat::BINARY);
        pd->SetHighOrderOutput(true);
        pd->SetCycle(0);
        pd->SetTime(0.0);
        pd->Save();
    }

    // Optionally output a BP (binary pack) file using ADIOS2. This can be
    // visualized with the ParaView VTX reader.
#ifdef MFEM_USE_ADIOS2
    ADIOS2DataCollection *adios2_dc = NULL;
    if (adios2)
    {
        std::string postfix(mesh_file);
        postfix.erase(0, std::string("../data/").size() );
        postfix += "_o" + std::to_string(order);
        const std::string collection_name = "dg_advection-p-" + postfix + ".bp";

        adios2_dc = new ADIOS2DataCollection(MPI_COMM_WORLD, collection_name, pmesh);
        // output data substreams are half the number of mpi processes
        adios2_dc->SetParameter("SubStreams", std::to_string(num_procs/2) );
        // adios2_dc->SetLevelsOfDetail(2);
        adios2_dc->RegisterField("solution", u);
        adios2_dc->SetCycle(0);
        adios2_dc->SetTime(0.0);
        adios2_dc->Save();
    }
#endif

    socketstream sout;
    if (visualization)
    {
        char vishost[] = "localhost";
        int  visport   = 19916;
        sout.open(vishost, visport);
        if (!sout)
        {
            if (myid == 0)
                cout << "Unable to connect to GLVis server at "
                     << vishost << ':' << visport << endl;
            visualization = false;
            if (myid == 0)
            {
                cout << "GLVis visualization disabled.\n";
            }
        }
        else
        {
            sout << "parallel " << num_procs << " " << myid << "\n";
            sout.precision(precision);
            sout << "solution\n" << *pmesh << *u;
            sout << flush;
        }
    }

    // 10. Define the time-dependent evolution operator describing the ODE
    //     right-hand side, and perform time-integration (looping over the time
    //     iterations, ti, with a time-step dt).
    FE_Evolution adv(*m, *k, *B, prec_type);

    StopWatch fom_timer, dmd_training_timer, dmd_prediction_timer;

    fom_timer.Start();

    double t = 0.0;
    vector<double> ts;
    adv.SetTime(t);
    ode_solver->Init(adv);

    fom_timer.Stop();

    dmd_training_timer.Start();

    // 11. Create DMD object and take initial sample.
    CAROM::DMD dmd_U(U->Size(), dt);
    dmd_U.takeSample(U->GetData(), t);
    ts.push_back(t);

    dmd_training_timer.Stop();

    bool done = false;
    for (int ti = 0; !done; )
    {

        fom_timer.Start();

        double dt_real = min(dt, t_final - t);
        ode_solver->Step(*U, t, dt_real);
        ti++;
        done = (t >= t_final - 1e-8*dt);

        fom_timer.Stop();

        dmd_training_timer.Start();

        dmd_U.takeSample(U->GetData(), t);
        ts.push_back(t);

        dmd_training_timer.Stop();

        if (done || ti % vis_steps == 0)
        {
            if (myid == 0)
            {
                cout << "time step: " << ti << ", time: " << t << endl;
            }

            // 11. Extract the parallel grid function corresponding to the finite
            //     element approximation U (the local solution on each processor).
            *u = *U;

            if (visualization)
            {
                sout << "parallel " << num_procs << " " << myid << "\n";
                sout << "solution\n" << *pmesh << *u << flush;
            }

            if (visit)
            {
                dc->SetCycle(ti);
                dc->SetTime(t);
                dc->Save();
            }

            if (paraview)
            {
                pd->SetCycle(ti);
                pd->SetTime(t);
                pd->Save();
            }

#ifdef MFEM_USE_ADIOS2
            // transient solutions can be visualized with ParaView
            if (adios2)
            {
                adios2_dc->SetCycle(ti);
                adios2_dc->SetTime(t);
                adios2_dc->Save();
            }
#endif
        }
    }

    // 12. Save the final solution in parallel. This output can be viewed later
    //     using GLVis: "glvis -np <np> -m dg_advection-mesh -g dg_advection-final".
    {
        *u = *U;
        ostringstream sol_name;
        sol_name << "dg_advection-final." << setfill('0') << setw(6) << myid;
        ofstream osol(sol_name.str().c_str());
        osol.precision(precision);
        u->Save(osol);
    }

    // 13. Calculate the DMD modes.
    if (myid == 0 && rdim != -1 && ef != -1)
    {
        std::cout << "Both rdim and ef are set. ef will be ignored." << std::endl;
    }

    dmd_training_timer.Start();

    if (rdim != -1)
    {
        if (myid == 0)
        {
            std::cout << "Creating DMD with rdim: " << rdim << std::endl;
        }
        dmd_U.train(rdim);
    }
    else if (ef != -1)
    {
        if (myid == 0)
        {
            std::cout << "Creating DMD with energy fraction: " << ef << std::endl;
        }
        dmd_U.train(ef);
    }

    dmd_training_timer.Stop();

    Vector true_solution_u(U->Size());
    true_solution_u = U->GetData();

    // 14. Predict using DMD.
    if (myid == 0)
    {
        std::cout << "Predicting solution using DMD" << std::endl;
    }

    CAROM::Vector* result_u = dmd_U.predict(ts[0]);
    Vector initial_dmd_solution_u(result_u->getData(), result_u->dim());
    u->SetFromTrueDofs(initial_dmd_solution_u);

    DataCollection *dmd_dc = NULL;
    if (visit)
    {
        dmd_dc = new VisItDataCollection("DMD_DG_Advection", pmesh);
        dmd_dc->SetPrecision(precision);
        // To save the mesh using MFEM's parallel mesh format:
        // dc->SetFormat(DataCollection::PARALLEL_FORMAT);
        dmd_dc->RegisterField("solution", u);
        dmd_dc->SetCycle(0);
        dmd_dc->SetTime(0.0);
        dmd_dc->Save();
    }

    delete result_u;

    if (visit)
    {
        for (int i = 1; i < ts.size(); i++)
        {
            if (i == ts.size() - 1 || (i % vis_steps) == 0)
            {
                result_u = dmd_U.predict(ts[i]);
                Vector dmd_solution_u(result_u->getData(), result_u->dim());
                u->SetFromTrueDofs(dmd_solution_u);

                dmd_dc->SetCycle(i);
                dmd_dc->SetTime(ts[i]);
                dmd_dc->Save();
                delete result_u;
            }
        }
    }

    dmd_prediction_timer.Start();
    result_u = dmd_U.predict(t_final);
    dmd_prediction_timer.Stop();

    // 15. Calculate the relative error between the DMD final solution and the true solution.
    Vector dmd_solution_u(result_u->getData(), result_u->dim());
    Vector diff_u(true_solution_u.Size());
    subtract(dmd_solution_u, true_solution_u, diff_u);

    double tot_diff_norm_u = sqrt(InnerProduct(MPI_COMM_WORLD, diff_u, diff_u));
    double tot_true_solution_u_norm = sqrt(InnerProduct(MPI_COMM_WORLD,
                                           true_solution_u, true_solution_u));

    if (myid == 0)
    {
        std::cout << "Relative error of DMD solution (u) at t_final: " << t_final <<
                  " is " << tot_diff_norm_u / tot_true_solution_u_norm << std::endl;
        printf("Elapsed time for solving FOM: %e second\n", fom_timer.RealTime());
        printf("Elapsed time for training DMD: %e second\n",
               dmd_training_timer.RealTime());
        printf("Elapsed time for predicting DMD: %e second\n",
               dmd_prediction_timer.RealTime());
    }

    // 16. Free the used memory.
    delete U;
    delete u;
    delete B;
    delete b;
    delete k;
    delete m;
    delete fes;
    delete pmesh;
    delete ode_solver;
    delete pd;
    delete result_u;
#ifdef MFEM_USE_ADIOS2
    if (adios2)
    {
        delete adios2_dc;
    }
#endif
    delete dc;
    delete dmd_dc;

    return 0;
}

// Implementation of class FE_Evolution
FE_Evolution::FE_Evolution(ParBilinearForm &M_, ParBilinearForm &K_,
                           const Vector &b_, PrecType prec_type)
    : TimeDependentOperator(M_.Height()), b(b_),
      M_solver(M_.ParFESpace()->GetComm()),
      z(M_.Height())
{
    if (M_.GetAssemblyLevel()==AssemblyLevel::LEGACY)
    {
        M.Reset(M_.ParallelAssemble(), true);
        K.Reset(K_.ParallelAssemble(), true);
    }
    else
    {
        M.Reset(&M_, false);
        K.Reset(&K_, false);
    }

    M_solver.SetOperator(*M);

    Array<int> ess_tdof_list;
    if (M_.GetAssemblyLevel()==AssemblyLevel::LEGACY)
    {
        HypreParMatrix &M_mat = *M.As<HypreParMatrix>();
        HypreParMatrix &K_mat = *K.As<HypreParMatrix>();
        HypreSmoother *hypre_prec = new HypreSmoother(M_mat, HypreSmoother::Jacobi);
        M_prec = hypre_prec;

        dg_solver = new DG_Solver(M_mat, K_mat, *M_.FESpace(), prec_type);
    }
    else
    {
        M_prec = new OperatorJacobiSmoother(M_, ess_tdof_list);
        dg_solver = NULL;
    }

    M_solver.SetPreconditioner(*M_prec);
    M_solver.iterative_mode = false;
    M_solver.SetRelTol(1e-9);
    M_solver.SetAbsTol(0.0);
    M_solver.SetMaxIter(100);
    M_solver.SetPrintLevel(0);
}

// Solve the equation:
//    u_t = M^{-1}(Ku + b),
// by solving associated linear system
//    (M - dt*K) d = K*u + b
void FE_Evolution::ImplicitSolve(const double dt, const Vector &x, Vector &k)
{
    K->Mult(x, z);
    z += b;
    dg_solver->SetTimeStep(dt);
    dg_solver->Mult(z, k);
}

void FE_Evolution::Mult(const Vector &x, Vector &y) const
{
    // y = M^{-1} (K x + b)
    K->Mult(x, z);
    z += b;
    M_solver.Mult(z, y);
}

FE_Evolution::~FE_Evolution()
{
    delete M_prec;
    delete dg_solver;
}

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v)
{
    int dim = x.Size();

    // map to the reference [-1,1] domain
    Vector X(dim);
    for (int i = 0; i < dim; i++)
    {
        double center = (bb_min[i] + bb_max[i]) * 0.5;
        X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
    }

    switch (problem)
    {
    case 0:
    {
        // Translations in 1D, 2D, and 3D
        switch (dim)
        {
        case 1:
            v(0) = 1.0;
            break;
        case 2:
            v(0) = sqrt(2./3.);
            v(1) = sqrt(1./3.);
            break;
        case 3:
            v(0) = sqrt(3./6.);
            v(1) = sqrt(2./6.);
            v(2) = sqrt(1./6.);
            break;
        }
        break;
    }
    case 1:
    case 2:
    {
        // Clockwise rotation in 2D around the origin
        const double w = M_PI/2;
        switch (dim)
        {
        case 1:
            v(0) = 1.0;
            break;
        case 2:
            v(0) = w*X(1);
            v(1) = -w*X(0);
            break;
        case 3:
            v(0) = w*X(1);
            v(1) = -w*X(0);
            v(2) = 0.0;
            break;
        }
        break;
    }
    case 3:
    {
        // Clockwise twisting rotation in 2D around the origin
        const double w = M_PI/2;
        double d = max((X(0)+1.)*(1.-X(0)),0.) * max((X(1)+1.)*(1.-X(1)),0.);
        d = d*d;
        switch (dim)
        {
        case 1:
            v(0) = 1.0;
            break;
        case 2:
            v(0) = d*w*X(1);
            v(1) = -d*w*X(0);
            break;
        case 3:
            v(0) = d*w*X(1);
            v(1) = -d*w*X(0);
            v(2) = 0.0;
            break;
        }
        break;
    }
    }
}

// Initial condition
double u0_function(const Vector &x)
{
    int dim = x.Size();

    // map to the reference [-1,1] domain
    Vector X(dim);
    for (int i = 0; i < dim; i++)
    {
        double center = (bb_min[i] + bb_max[i]) * 0.5;
        X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
    }

    switch (problem)
    {
    case 0:
    case 1:
    {
        switch (dim)
        {
        case 1:
            return exp(-40.*pow(X(0)-0.5,2));
        case 2:
        case 3:
        {
            double rx = 0.45, ry = 0.25, cx = 0., cy = -0.2, w = 10.;
            if (dim == 3)
            {
                const double s = (1. + 0.25*cos(2*M_PI*X(2)));
                rx *= s;
                ry *= s;
            }
            return ( erfc(w*(X(0)-cx-rx))*erfc(-w*(X(0)-cx+rx)) *
                     erfc(w*(X(1)-cy-ry))*erfc(-w*(X(1)-cy+ry)) )/16;
        }
        }
    }
    case 2:
    {
        double x_ = X(0), y_ = X(1), rho, phi;
        rho = hypot(x_, y_);
        phi = atan2(y_, x_);
        return pow(sin(M_PI*rho),2)*sin(3*phi);
    }
    case 3:
    {
        const double f = M_PI;
        return sin(f*X(0))*sin(f*X(1));
    }
    }
    return 0.0;
}

// Inflow boundary condition (zero for the problems considered in this example)
double inflow_function(const Vector &x)
{
    switch (problem)
    {
    case 0:
    case 1:
    case 2:
    case 3:
        return 0.0;
    }
    return 0.0;
}
