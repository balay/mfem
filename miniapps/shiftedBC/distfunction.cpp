// Copyright (c) 2010-2020, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "distfunction.hpp"

using namespace mfem;

DistanceFunction::DistanceFunction(ParMesh &pmesh, int order,
                                   double diff_coeff, bool use_amgx_)
   : fec(order, pmesh.Dimension()),
     pfes(&pmesh, &fec),
     distance(&pfes), source(&pfes), diffused_source(&pfes),
     t_param(diff_coeff), use_amgx(use_amgx_)
{
   // Compute average mesh size (assumes similar cells).
   double loc_area = 0.0;
   for (int i = 0; i < pmesh.GetNE(); i++)
   {
      loc_area += pmesh.GetElementVolume(i);
   }
   double glob_area;
   MPI_Allreduce(&loc_area, &glob_area, 1, MPI_DOUBLE,
                 MPI_SUM, pfes.GetComm());

   const int glob_zones = pmesh.GetGlobalNE();
   switch (pmesh.GetElementBaseGeometry(0))
   {
      case Geometry::SEGMENT:
         dx = glob_area / glob_zones; break;
      case Geometry::SQUARE:
         dx = sqrt(glob_area / glob_zones); break;
      case Geometry::TRIANGLE:
         dx = sqrt(2.0 * glob_area / glob_zones); break;
      case Geometry::CUBE:
         dx = pow(glob_area / glob_zones, 1.0/3.0); break;
      case Geometry::TETRAHEDRON:
         dx = pow(6.0 * glob_area / glob_zones, 1.0/3.0); break;
      default: MFEM_ABORT("Unknown zone type!");
   }
   dx /= order;


   // List of true essential boundary dofs.
   if (pmesh.bdr_attributes.Size())
   {
      Array<int> ess_bdr(pmesh.bdr_attributes.Max());
      ess_bdr = 1;
      pfes.GetEssentialTrueDofs(ess_bdr, ess_tdof_list);
   }
}

void DiffuseField(ParGridFunction &field, int smooth_steps)
{
   // Setup the Laplacian operator.
   ParBilinearForm *Lap = new ParBilinearForm(field.ParFESpace());
   Lap->AddDomainIntegrator(new DiffusionIntegrator());
   Lap->Assemble();
   Lap->Finalize();
   HypreParMatrix *A = Lap->ParallelAssemble();

   HypreSmoother *S = new HypreSmoother(*A,0,smooth_steps);
   S->iterative_mode = true;

   Vector tmp(A->Width());
   field.SetTrueVector();
   Vector fieldtrue = field.GetTrueVector();
   tmp = 0.0;
   S->Mult(tmp, fieldtrue);

   field.SetFromTrueDofs(fieldtrue);

   delete S;
   delete Lap;
}

Solver * GetPrecondtioner(bool use_amgx)
{
  Solver *prec; bool amgx_verbosity(false);
#if defined(MFEM_USE_AMGX)
  if(use_amgx) {
    prec = new AmgXSolver(MPI_COMM_WORLD,
                          AmgXSolver::PRECONDITIONER,
                          amgx_verbosity);
  }else {
    prec = new HypreBoomerAMG;
  }
#else
  if(use_amgx) {mfem_error("AMGX not enabled \n");}
  prec = new HypreBoomerAMG;
#endif

    return prec;
}

ParGridFunction &DistanceFunction::ComputeDistance(Coefficient &level_set,
                                                   int smooth_steps,
                                                   bool transform)
{
   source.ProjectCoefficient(level_set);

   // Optional smoothing of the initial level set.
   if (smooth_steps > 0) { DiffuseField(source, smooth_steps); }

   // Transform so that the peak is at 0.
   // Assumes range [0, 1].
   if (transform)
   {
      for (int i = 0; i < source.Size(); i++)
      {
         const double x = source(i);
         source(i) = ((x < 0.0) || (x > 1.0)) ? 0.0 : 4.0 * x * (1.0 - x);
      }
   }

   // Solver.
   CGSolver cg(MPI_COMM_WORLD);
   cg.SetRelTol(1e-12);
   cg.SetMaxIter(100);
   cg.SetPrintLevel(1);
   OperatorPtr A;
   Vector B, X;

   // Step 1 - diffuse.
   {
      // Set up RHS.
      ParLinearForm b1(&pfes);
      GridFunctionCoefficient src_coeff(&source);
      b1.AddDomainIntegrator(new DomainLFIntegrator(src_coeff));
      b1.Assemble();

      // Diffusion and mass terms in the LHS.
      ParBilinearForm a1(&pfes);
      a1.AddDomainIntegrator(new MassIntegrator);
      const double dt = t_param * dx * dx;
      ConstantCoefficient t_coeff(dt);
      a1.AddDomainIntegrator(new DiffusionIntegrator(t_coeff));
      a1.Assemble();

      // Solve with Dirichlet BC.
      ParGridFunction u_dirichlet(&pfes);
      u_dirichlet = 0.0;
      a1.FormLinearSystem(ess_tdof_list, u_dirichlet, b1, A, X, B);
      Solver *prec = GetPrecondtioner(use_amgx);
      cg.SetPreconditioner(*prec);
      cg.SetOperator(*A);
      cg.Mult(B, X);
      a1.RecoverFEMSolution(X, b1, u_dirichlet);
      delete prec;

      // Diffusion and mass terms in the LHS.
      ParBilinearForm a_n(&pfes);
      a_n.AddDomainIntegrator(new MassIntegrator);
      a_n.AddDomainIntegrator(new DiffusionIntegrator(t_coeff));
      a_n.Assemble();

      // Solve with Neumann BC.
      ParGridFunction u_neumann(&pfes);
      ess_tdof_list.DeleteAll();
      a_n.FormLinearSystem(ess_tdof_list, u_neumann, b1, A, X, B);

      Solver *prec2 = GetPrecondtioner(use_amgx);

      cg.SetPreconditioner(*prec2);
      cg.SetOperator(*A);
      cg.Mult(B, X);
      a_n.RecoverFEMSolution(X, b1, u_neumann);
      delete prec2;

      for (int i = 0; i < diffused_source.Size(); i++)
      {
         diffused_source(i) = 0.5 * (u_neumann(i) + u_dirichlet(i));
      }
   }

   // Step 2 - solve for the distance using the normalized gradient.
   {
      // RHS - normalized gradient.
      ParLinearForm b2(&pfes);
      GradientCoefficient grad_u(diffused_source,
                                 pfes.GetMesh()->Dimension());
      b2.AddDomainIntegrator(new DomainLFGradIntegrator(grad_u));
      b2.Assemble();

      // LHS - diffusion.
      ParBilinearForm a2(&pfes);
      a2.AddDomainIntegrator(new DiffusionIntegrator);
      a2.Assemble();

      // No BC.
      Array<int> no_ess_tdofs;

      a2.FormLinearSystem(no_ess_tdofs, distance, b2, A, X, B);

      Solver *prec2 = GetPrecondtioner(use_amgx);
      cg.SetPreconditioner(*prec2);
      cg.SetOperator(*A);
      cg.Mult(B, X);
      a2.RecoverFEMSolution(X, b2, distance);
      delete prec2;
   }

   // Rescale the distance to have minimum at zero.
   double d_min_loc = distance.Min();
   double d_min_glob;
   MPI_Allreduce(&d_min_loc, &d_min_glob, 1, MPI_DOUBLE,
                 MPI_MIN, pfes.GetComm());
   distance -= d_min_glob;

   return distance;
}
