/*
 * Author: Charlie Zhang
 */
#include <deal.II/base/quadrature_lib.h>
#include <deal.II/base/function.h>
#include <deal.II/base/logstream.h>
#include <deal.II/base/utilities.h>
#include <deal.II/lac/vector.h>
#include <deal.II/lac/full_matrix.h>
#include <deal.II/lac/sparse_matrix.h>
#include <deal.II/lac/compressed_sparsity_pattern.h>
#include <deal.II/lac/solver_cg.h>
#include <deal.II/lac/precondition.h>
#include <deal.II/lac/constraint_matrix.h>
#include <deal.II/grid/tria.h>
#include <deal.II/grid/grid_generator.h>
#include <deal.II/grid/tria_accessor.h>
#include <deal.II/grid/tria_iterator.h>
#include <deal.II/grid/tria_boundary_lib.h>
#include <deal.II/grid/grid_refinement.h>
#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_accessor.h>
#include <deal.II/dofs/dof_tools.h>
#include <deal.II/fe/fe_values.h>
#include <deal.II/fe/fe_q.h>
#include <deal.II/numerics/vector_tools.h>
#include <deal.II/numerics/matrix_tools.h>
#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/error_estimator.h>
#include <fstream>
#include <iostream>
#include <deal.II/numerics/solution_transfer.h>
//add solver
#include <deal.II/lac/sparse_direct.h>
 #include <deal.II/lac/solver_gmres.h>
namespace Test
{
  using namespace dealii;
  template <int dim>
  class MinimalSurfaceProblem
  {
  public:
    MinimalSurfaceProblem ();
    ~MinimalSurfaceProblem ();
    void run ();
  private:
    void setup_system (const bool initial_step);
    void assemble_system ();
    void solve ();
    void refine_mesh ();
    void set_boundary_values ();
    double compute_residual (const double alpha) const;
    double determine_step_length () const;
    Triangulation<dim>   triangulation;
    DoFHandler<dim>      dof_handler;
    FE_Q<dim>            fe;
    ConstraintMatrix     hanging_node_constraints;
    SparsityPattern      sparsity_pattern;
    SparseMatrix<double> system_matrix;
    Vector<double>       present_solution;
    Vector<double>       newton_update;
    Vector<double>       system_rhs;
  };
  template <int dim>
  class BoundaryValues : public Function<dim>
  {
  public:
    BoundaryValues () : Function<dim>() {}
    virtual double value (const Point<dim>   &p,
                          const unsigned int  component = 0) const;
  };
  template <int dim>
  double BoundaryValues<dim>::value (const Point<dim> &p,
                                     const unsigned int /*component*/) const
  {
    return std::sin(2 * numbers::PI * (p[0]+p[1]));
  }
  template <int dim>
  MinimalSurfaceProblem<dim>::MinimalSurfaceProblem ()
    :
    dof_handler (triangulation),
    fe (2)
  {}
  template <int dim>
  MinimalSurfaceProblem<dim>::~MinimalSurfaceProblem ()
  {
    dof_handler.clear ();
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::setup_system (const bool initial_step)
  {
    if (initial_step)
      {
        dof_handler.distribute_dofs (fe);
        present_solution.reinit (dof_handler.n_dofs());
        hanging_node_constraints.clear ();
        DoFTools::make_hanging_node_constraints (dof_handler,
                                                 hanging_node_constraints);
        hanging_node_constraints.close ();
      }
    newton_update.reinit (dof_handler.n_dofs());
    system_rhs.reinit (dof_handler.n_dofs());
    CompressedSparsityPattern c_sparsity(dof_handler.n_dofs());
    DoFTools::make_sparsity_pattern (dof_handler, c_sparsity);
    hanging_node_constraints.condense (c_sparsity);
    sparsity_pattern.copy_from(c_sparsity);
    system_matrix.reinit (sparsity_pattern);
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::assemble_system ()
  {
    const QGauss<dim>  quadrature_formula(3);
    system_matrix = 0;
    system_rhs = 0;
    FEValues<dim> fe_values (fe, quadrature_formula,
                             update_gradients         |
                             update_quadrature_points |
                             update_JxW_values);
    const unsigned int           dofs_per_cell = fe.dofs_per_cell;
    const unsigned int           n_q_points    = quadrature_formula.size();
    FullMatrix<double>           cell_matrix (dofs_per_cell, dofs_per_cell);
    Vector<double>               cell_rhs (dofs_per_cell);
    std::vector<Tensor<1, dim> > old_solution_gradients(n_q_points);
    std::vector<types::global_dof_index>    local_dof_indices (dofs_per_cell);
    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell!=endc; ++cell)
      {
        cell_matrix = 0;
        cell_rhs = 0;
        fe_values.reinit (cell);
        fe_values.get_function_gradients(present_solution,
                                         old_solution_gradients);
        for (unsigned int q_point = 0; q_point < n_q_points; ++q_point)
          {
            const double coeff
              = 1.0 / std::sqrt(1 +
                                old_solution_gradients[q_point] *
                                old_solution_gradients[q_point]);
            for (unsigned int i=0; i<dofs_per_cell; ++i)
              {
                for (unsigned int j=0; j<dofs_per_cell; ++j)
                  {
                    cell_matrix(i, j) += (fe_values.shape_grad(i, q_point)
                                          * coeff
                                          * (fe_values.shape_grad(j, q_point)
                                             -
                                             coeff * coeff
                                             * (fe_values.shape_grad(j, q_point)
                                                *
                                                old_solution_gradients[q_point])
                                             * old_solution_gradients[q_point]
                                            )
                                          * fe_values.JxW(q_point));
                  }
                cell_rhs(i) -= (fe_values.shape_grad(i, q_point)
                                * coeff
                                * old_solution_gradients[q_point]
                                * fe_values.JxW(q_point));
              }
          }
        cell->get_dof_indices (local_dof_indices);
        for (unsigned int i=0; i<dofs_per_cell; ++i)
          {
            for (unsigned int j=0; j<dofs_per_cell; ++j)
              system_matrix.add (local_dof_indices[i],
                                 local_dof_indices[j],
                                 cell_matrix(i,j));
            system_rhs(local_dof_indices[i]) += cell_rhs(i);
          }
      }
    hanging_node_constraints.condense (system_matrix);
    hanging_node_constraints.condense (system_rhs);
    std::map<types::global_dof_index,double> boundary_values;
    VectorTools::interpolate_boundary_values (dof_handler,
                                              0,
                                              ZeroFunction<dim>(),
                                              boundary_values);
    MatrixTools::apply_boundary_values (boundary_values,
                                        system_matrix,
                                        newton_update,
                                        system_rhs);
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::solve ()
  {
    SolverControl solver_control (system_rhs.size(),
                                  system_rhs.l2_norm()*1e-6);
    SolverCG<>    solver (solver_control);
    PreconditionSSOR<> preconditioner;
    preconditioner.initialize(system_matrix, 1.2);
    solver.solve (system_matrix, newton_update, system_rhs,
                  preconditioner);
    hanging_node_constraints.distribute (newton_update);
    const double alpha = determine_step_length();
    present_solution.add (alpha, newton_update);
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::refine_mesh ()
  {
    Vector<float> estimated_error_per_cell (triangulation.n_active_cells());
    KellyErrorEstimator<dim>::estimate (dof_handler,
                                        QGauss<dim-1>(3),
                                        typename FunctionMap<dim>::type(),
                                        present_solution,
                                        estimated_error_per_cell);
    GridRefinement::refine_and_coarsen_fixed_number (triangulation,
                                                     estimated_error_per_cell,
                                                     0.3, 0.03);
    triangulation.prepare_coarsening_and_refinement ();
    SolutionTransfer<dim> solution_transfer(dof_handler);
    solution_transfer.prepare_for_coarsening_and_refinement(present_solution);
    triangulation.execute_coarsening_and_refinement();
    dof_handler.distribute_dofs(fe);
    Vector<double> tmp(dof_handler.n_dofs());
    solution_transfer.interpolate(present_solution, tmp);
    present_solution = tmp;
    set_boundary_values ();
    hanging_node_constraints.clear();
    DoFTools::make_hanging_node_constraints(dof_handler,
                                            hanging_node_constraints);
    hanging_node_constraints.close();
    hanging_node_constraints.distribute (present_solution);
    setup_system (false);
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::set_boundary_values ()
  {
    std::map<types::global_dof_index, double> boundary_values;
    VectorTools::interpolate_boundary_values (dof_handler,
                                              0,
                                              BoundaryValues<dim>(),
                                              boundary_values);
    for (std::map<types::global_dof_index, double>::const_iterator
         p = boundary_values.begin();
         p != boundary_values.end(); ++p)
      present_solution(p->first) = p->second;
  }
  template <int dim>
  double MinimalSurfaceProblem<dim>::compute_residual (const double alpha) const
  {
    Vector<double> residual (dof_handler.n_dofs());
    Vector<double> evaluation_point (dof_handler.n_dofs());
    evaluation_point = present_solution;
    evaluation_point.add (alpha, newton_update);
    const QGauss<dim>  quadrature_formula(3);
    FEValues<dim> fe_values (fe, quadrature_formula,
                             update_gradients         |
                             update_quadrature_points |
                             update_JxW_values);
    const unsigned int           dofs_per_cell = fe.dofs_per_cell;
    const unsigned int           n_q_points    = quadrature_formula.size();
    Vector<double>               cell_rhs (dofs_per_cell);
    std::vector<Tensor<1, dim> > gradients(n_q_points);
    std::vector<types::global_dof_index>    local_dof_indices (dofs_per_cell);
    typename DoFHandler<dim>::active_cell_iterator
    cell = dof_handler.begin_active(),
    endc = dof_handler.end();
    for (; cell!=endc; ++cell)
      {
        cell_rhs = 0;
        fe_values.reinit (cell);
        fe_values.get_function_gradients (evaluation_point,
                                          gradients);
        for (unsigned int q_point=0; q_point<n_q_points; ++q_point)
          {
            const double coeff = 1/std::sqrt(1 +
                                             gradients[q_point] *
                                             gradients[q_point]);
            for (unsigned int i = 0; i < dofs_per_cell; ++i)
              cell_rhs(i) -= (fe_values.shape_grad(i, q_point)
                              * coeff
                              * gradients[q_point]
                              * fe_values.JxW(q_point));
          }
        cell->get_dof_indices (local_dof_indices);
        for (unsigned int i=0; i<dofs_per_cell; ++i)
          residual(local_dof_indices[i]) += cell_rhs(i);
      }
    hanging_node_constraints.condense (residual);
    std::vector<bool> boundary_dofs (dof_handler.n_dofs());
    DoFTools::extract_boundary_dofs (dof_handler,
                                     ComponentMask(),
                                     boundary_dofs);
    for (unsigned int i=0; i<dof_handler.n_dofs(); ++i)
      if (boundary_dofs[i] == true)
        residual(i) = 0;
    return residual.l2_norm();
  }
  template <int dim>
  double MinimalSurfaceProblem<dim>::determine_step_length() const
  {
    return 0.1;
  }
  template <int dim>
  void MinimalSurfaceProblem<dim>::run ()
  {
    unsigned int refinement = 0;
    bool         first_step = true;
    GridGenerator::hyper_ball (triangulation);
    static const HyperBallBoundary<dim> boundary;
    triangulation.set_boundary (0, boundary);
    triangulation.refine_global(3);
    double previous_res = 1;
    for (unsigned int inner_iteration=0; (inner_iteration<1000)&&(previous_res>1e-3); ++inner_iteration)
      {
        if (first_step == true)
          {
            std::cout << "******** Initial mesh "
                      << " ********"
                      << std::endl;
            setup_system (true);
            set_boundary_values ();
          }
        assemble_system ();
        previous_res = system_rhs.l2_norm();
        solve ();
        first_step = false;
        std::cout << "  Residual: "
                  << compute_residual(0)
                  << std::endl;
        if (inner_iteration%10==0)
        {
          DataOut<dim> data_out;
          data_out.attach_dof_handler (dof_handler);
          data_out.add_data_vector (present_solution, "solution");
          data_out.add_data_vector (newton_update, "update");
          data_out.build_patches ();
          const std::string filename = "solution-" +
                                       Utilities::int_to_string (inner_iteration/10, 3) +
                                       ".vtk";
          std::ofstream output (filename.c_str());
          data_out.write_vtk (output);
        }
      }

  }
}
int main ()
{
  try
    {
      using namespace dealii;
      using namespace Test;
      deallog.depth_console (0);
      MinimalSurfaceProblem<2> laplace_problem_2d;
      laplace_problem_2d.run ();
    }
  catch (std::exception &exc)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Exception on processing: " << std::endl
                << exc.what() << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  catch (...)
    {
      std::cerr << std::endl << std::endl
                << "----------------------------------------------------"
                << std::endl;
      std::cerr << "Unknown exception!" << std::endl
                << "Aborting!" << std::endl
                << "----------------------------------------------------"
                << std::endl;
      return 1;
    }
  return 0;
}
