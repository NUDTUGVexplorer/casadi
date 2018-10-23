/*
 *    This file is part of CasADi.
 *
 *    CasADi -- A symbolic framework for dynamic optimization.
 *    Copyright (C) 2010-2014 Joel Andersson, Joris Gillis, Moritz Diehl,
 *                            K.U. Leuven. All rights reserved.
 *    Copyright (C) 2011-2014 Greg Horn
 *
 *    CasADi is free software; you can redistribute it and/or
 *    modify it under the terms of the GNU Lesser General Public
 *    License as published by the Free Software Foundation; either
 *    version 3 of the License, or (at your option) any later version.
 *
 *    CasADi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *    Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with CasADi; if not, write to the Free Software
 *    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */


#include "sqpmethod.hpp"

#include "casadi/core/casadi_misc.hpp"
#include "casadi/core/calculus.hpp"
#include "casadi/core/conic.hpp"

#include <ctime>
#include <iomanip>
#include <fstream>
#include <cmath>
#include <cfloat>

using namespace std;
namespace casadi {

  extern "C"
  int CASADI_NLPSOL_SQPMETHOD_EXPORT
      casadi_register_nlpsol_sqpmethod(Nlpsol::Plugin* plugin) {
    plugin->creator = Sqpmethod::creator;
    plugin->name = "sqpmethod";
    plugin->doc = Sqpmethod::meta_doc.c_str();
    plugin->version = CASADI_VERSION;
    plugin->options = &Sqpmethod::options_;
    plugin->deserialize = &Sqpmethod::deserialize;
    return 0;
  }

  extern "C"
  void CASADI_NLPSOL_SQPMETHOD_EXPORT casadi_load_nlpsol_sqpmethod() {
    Nlpsol::registerPlugin(casadi_register_nlpsol_sqpmethod);
  }

  Sqpmethod::Sqpmethod(const std::string& name, const Function& nlp)
    : Nlpsol(name, nlp) {
  }

  Sqpmethod::~Sqpmethod() {
    clear_mem();
  }

  const Options Sqpmethod::options_
  = {{&Nlpsol::options_},
     {{"qpsol",
       {OT_STRING,
        "The QP solver to be used by the SQP method [qpoases]"}},
      {"qpsol_options",
       {OT_DICT,
        "Options to be passed to the QP solver"}},
      {"hessian_approximation",
       {OT_STRING,
        "limited-memory|exact"}},
      {"max_iter",
       {OT_INT,
        "Maximum number of SQP iterations"}},
      {"min_iter",
       {OT_INT,
        "Minimum number of SQP iterations"}},
      {"max_iter_ls",
       {OT_INT,
        "Maximum number of linesearch iterations"}},
      {"tol_pr",
       {OT_DOUBLE,
        "Stopping criterion for primal infeasibility"}},
      {"tol_du",
       {OT_DOUBLE,
        "Stopping criterion for dual infeasability"}},
      {"c1",
       {OT_DOUBLE,
        "Armijo condition, coefficient of decrease in merit"}},
      {"beta",
       {OT_DOUBLE,
        "Line-search parameter, restoration factor of stepsize"}},
      {"merit_memory",
       {OT_INT,
        "Size of memory to store history of merit function values"}},
      {"lbfgs_memory",
       {OT_INT,
        "Size of L-BFGS memory."}},
      {"regularize",
       {OT_BOOL,
        "Automatic regularization of Lagrange Hessian."}},
      {"print_header",
       {OT_BOOL,
        "Print the header with problem statistics"}},
      {"print_iteration",
       {OT_BOOL,
        "Print the iterations"}},
      {"print_status",
       {OT_BOOL,
        "Print a status message after solving"}},
      {"min_step_size",
       {OT_DOUBLE,
        "The size (inf-norm) of the step size should not become smaller than this."}},
     }
  };

  void Sqpmethod::init(const Dict& opts) {
    // Call the init method of the base class
    Nlpsol::init(opts);

    // Default options
    min_iter_ = 0;
    max_iter_ = 50;
    max_iter_ls_ = 3;
    c1_ = 1e-4;
    beta_ = 0.8;
    merit_memsize_ = 4;
    lbfgs_memory_ = 10;
    tol_pr_ = 1e-6;
    tol_du_ = 1e-6;
    regularize_ = false;
    string hessian_approximation = "exact";
    min_step_size_ = 1e-10;
    string qpsol_plugin = "qpoases";
    Dict qpsol_options;
    print_header_ = true;
    print_iteration_ = true;
    print_status_ = true;

    // Read user options
    for (auto&& op : opts) {
      if (op.first=="max_iter") {
        max_iter_ = op.second;
      } else if (op.first=="min_iter") {
        min_iter_ = op.second;
      } else if (op.first=="max_iter_ls") {
        max_iter_ls_ = op.second;
      } else if (op.first=="c1") {
        c1_ = op.second;
      } else if (op.first=="beta") {
        beta_ = op.second;
      } else if (op.first=="merit_memory") {
        merit_memsize_ = op.second;
      } else if (op.first=="lbfgs_memory") {
        lbfgs_memory_ = op.second;
      } else if (op.first=="tol_pr") {
        tol_pr_ = op.second;
      } else if (op.first=="tol_du") {
        tol_du_ = op.second;
      } else if (op.first=="hessian_approximation") {
        hessian_approximation = op.second.to_string();
      } else if (op.first=="min_step_size") {
        min_step_size_ = op.second;
      } else if (op.first=="qpsol") {
        qpsol_plugin = op.second.to_string();
      } else if (op.first=="qpsol_options") {
        qpsol_options = op.second;
      } else if (op.first=="regularize") {
        regularize_ = op.second;
      } else if (op.first=="print_header") {
        print_header_ = op.second;
      } else if (op.first=="print_iteration") {
        print_iteration_ = op.second;
      } else if (op.first=="print_status") {
        print_status_ = op.second;
      }
    }

    // Use exact Hessian?
    exact_hessian_ = hessian_approximation =="exact";

    // Get/generate required functions
    if (max_iter_ls_) create_function("nlp_fg", {"x", "p"}, {"f", "g"});
    // First order derivative information
    Function jac_g_fcn = create_function("nlp_jac_fg", {"x", "p"},
                                        {"f", "grad:f:x", "g", "jac:g:x"});
    Asp_ = jac_g_fcn.sparsity_out(3);

    if (exact_hessian_) {
      Function hess_l_fcn = create_function("nlp_hess_l", {"x", "p", "lam:f", "lam:g"},
                                           {"sym:hess:gamma:x:x"}, {{"gamma", {"f", "g"}}});
      Hsp_ = hess_l_fcn.sparsity_out(0);
    } else {
      Hsp_ = Sparsity::dense(nx_, nx_);
    }


    // Allocate a QP solver
    casadi_assert(!qpsol_plugin.empty(), "'qpsol' option has not been set");
    qpsol_ = conic("qpsol", qpsol_plugin, {{"h", Hsp_}, {"a", Asp_}},
                   qpsol_options);
    alloc(qpsol_);

    // BFGS?
    if (!exact_hessian_) {
      alloc_w(2*nx_); // casadi_bfgs
    }

    // Header
    if (print_header_) {
      print("-------------------------------------------\n");
      print("This is casadi::Sqpmethod.\n");
      if (exact_hessian_) {
        print("Using exact Hessian\n");
      } else {
        print("Using limited memory BFGS Hessian approximation\n");
      }
      print("Number of variables:                       %9d\n", nx_);
      print("Number of constraints:                     %9d\n", ng_);
      print("Number of nonzeros in constraint Jacobian: %9d\n", Asp_.nnz());
      print("Number of nonzeros in Lagrangian Hessian:  %9d\n", Hsp_.nnz());
      print("\n");
    }


    set_sqpmethod_prob();
    // Allocate memory
    casadi_int sz_w, sz_iw;
    casadi_sqpmethod_work(&p_, &sz_iw, &sz_w);
    alloc_iw(sz_iw, true);
    alloc_w(sz_w, true);
  }

  void Sqpmethod::set_sqpmethod_prob() {
    p_.sp_h = Hsp_;
    p_.sp_a = Asp_;
    p_.merit_memsize = merit_memsize_;
    p_.max_iter_ls = max_iter_ls_;
    p_.nlp = &p_nlp_;
  }

  void Sqpmethod::set_work(void* mem, const double**& arg, double**& res,
                                casadi_int*& iw, double*& w) const {
    auto m = static_cast<SqpmethodMemory*>(mem);

    // Set work in base classes
    Nlpsol::set_work(mem, arg, res, iw, w);

    m->d.prob = &p_;
    casadi_sqpmethod_init(&m->d, &iw, &w);

    m->iter_count = -1;
  }

int Sqpmethod::solve(void* mem) const {
    auto m = static_cast<SqpmethodMemory*>(mem);
    auto d_nlp = &m->d_nlp;
    auto d = &m->d;

    // Number of SQP iterations
    m->iter_count = 0;

    // Number of line-search iterations
    casadi_int ls_iter = 0;

    // Last linesearch successfull
    bool ls_success = true;

    // Reset
    m->merit_ind = 0;
    m->sigma = 0.;    // NOTE: Move this into the main optimization loop
    m->reg = 0;

    // Default stepsize
    double t = 0;

    // For seeds
    const double one = 1.;

    casadi_fill(d->dx, nx_, 0.);

    // MAIN OPTIMIZATION LOOP
    while (true) {
      // Evaluate f, g and first order derivative information
      m->arg[0] = d_nlp->z;
      m->arg[1] = d_nlp->p;
      m->res[0] = &d_nlp->f;
      m->res[1] = d->gf;
      m->res[2] = d_nlp->z + nx_;
      m->res[3] = d->Jk;
      if (calc_function(m, "nlp_jac_fg")) return 1;

      // Evaluate the gradient of the Lagrangian
      casadi_copy(d->gf, nx_, d->gLag);
      casadi_mv(d->Jk, Asp_, d_nlp->lam+nx_, d->gLag, true);
      casadi_axpy(nx_, 1., d_nlp->lam, d->gLag);

      // Primal infeasability
      double pr_inf = casadi_max_viol(nx_+ng_, d_nlp->z, d_nlp->lbz, d_nlp->ubz);

      // inf-norm of Lagrange gradient
      double du_inf = casadi_norm_inf(nx_, d->gLag);

      // inf-norm of step
      double dx_norminf = casadi_norm_inf(nx_, d->dx);

      // Printing information about the actual iterate
      if (print_iteration_) {
        if (m->iter_count % 10 == 0) print_iteration();
        print_iteration(m->iter_count, d_nlp->f, pr_inf, du_inf, dx_norminf,
                        m->reg, ls_iter, ls_success);
      }

      // Callback function
      if (callback(m)) {
        if (print_status_) print("WARNING(sqpmethod): Aborted by callback...\n");
        m->return_status = "User_Requested_Stop";
        break;
      }

      // Checking convergence criteria
      if (m->iter_count >= min_iter_ && pr_inf < tol_pr_ && du_inf < tol_du_) {
        if (print_status_)
          print("MESSAGE(sqpmethod): Convergence achieved after %d iterations\n", m->iter_count);
        m->return_status = "Solve_Succeeded";
        m->success = true;
        break;
      }

      if (m->iter_count >= max_iter_) {
        if (print_status_) print("MESSAGE(sqpmethod): Maximum number of iterations reached.\n");
        m->return_status = "Maximum_Iterations_Exceeded";
        m->unified_return_status = SOLVER_RET_LIMITED;
        break;
      }

      if (m->iter_count >= 1 && m->iter_count >= min_iter_ && dx_norminf <= min_step_size_) {
        if (print_status_) print("MESSAGE(sqpmethod): Search direction becomes too small without "
              "convergence criteria being met.\n");
        m->return_status = "Search_Direction_Becomes_Too_Small";
        break;
      }

      if (exact_hessian_) {
        // Update/reset exact Hessian
        m->arg[0] = d_nlp->z;
        m->arg[1] = d_nlp->p;
        m->arg[2] = &one;
        m->arg[3] = d_nlp->lam + nx_;
        m->res[0] = d->Bk;
        if (calc_function(m, "nlp_hess_l")) return 1;

        // Determing regularization parameter with Gershgorin theorem
        if (regularize_) {
          m->reg = std::fmin(0, -casadi_lb_eig(Hsp_, d->Bk));
          if (m->reg > 0) casadi_regularize(Hsp_, d->Bk, m->reg);
        }
      } else if (m->iter_count==0) {
        // Initialize BFGS
        casadi_fill(d->Bk, Hsp_.nnz(), 1.);
        casadi_bfgs_reset(Hsp_, d->Bk);
      } else {
        // Update BFGS
        if (m->iter_count % lbfgs_memory_ == 0) casadi_bfgs_reset(Hsp_, d->Bk);
        // Update the Hessian approximation
        casadi_bfgs(Hsp_, d->Bk, d->dx, d->gLag, d->gLag_old, m->w);
      }

      // Formulate the QP
      casadi_copy(d_nlp->lbz, nx_+ng_, d->lbdz);
      casadi_axpy(nx_+ng_, -1., d_nlp->z, d->lbdz);
      casadi_copy(d_nlp->ubz, nx_+ng_, d->ubdz);
      casadi_axpy(nx_+ng_, -1., d_nlp->z, d->ubdz);

      // Initial guess
      casadi_copy(d_nlp->lam, nx_+ng_, d->dlam);
      casadi_fill(d->dx, nx_, 0.);

      // Increase counter
      m->iter_count++;

      // Solve the QP
      solve_QP(m, d->Bk, d->gf, d->lbdz, d->ubdz, d->Jk,
               d->dx, d->dlam);

      // Detecting indefiniteness
      double gain = casadi_bilin(d->Bk, Hsp_, d->dx, d->dx);
      if (gain < 0) {
        if (print_status_) print("WARNING(sqpmethod): Indefinite Hessian detected\n");
      }

      // Stepsize
      t = 1.0;
      double fk_cand;
      // Merit function value in candidate
      double L1merit_cand = 0;

      // Reset line-search counter, success marker
      ls_iter = 0;
      ls_success = true;

      // Line-search
      if (verbose_) print("Starting line-search\n");
      if (max_iter_ls_>0) { // max_iter_ls_== 0 disables line-search

        // Calculate penalty parameter of merit function
        m->sigma = std::fmax(m->sigma, 1.01*casadi_norm_inf(nx_+ng_, d->dlam));
        // Calculate L1-merit function in the actual iterate
        double l1_infeas = casadi_max_viol(nx_+ng_, d_nlp->z, d_nlp->lbz, d_nlp->ubz);
        // Right-hand side of Armijo condition
        double F_sens = casadi_dot(nx_, d->dx, d->gf);
        double L1dir = F_sens - m->sigma * l1_infeas;
        double L1merit = d_nlp->f + m->sigma * l1_infeas;
        // Storing the actual merit function value in a list
        d->merit_mem[m->merit_ind] = L1merit;
        m->merit_ind++;
        m->merit_ind %= merit_memsize_;
        // Calculating maximal merit function value so far
        double meritmax = casadi_vfmax(d->merit_mem+1,
          std::min(merit_memsize_, static_cast<casadi_int>(m->iter_count))-1, d->merit_mem[0]);

        // Line-search loop
        while (true) {
          // Increase counter
          ls_iter++;

          // Candidate step
          casadi_copy(d_nlp->z, nx_, d->z_cand);
          casadi_axpy(nx_, t, d->dx, d->z_cand);

          // Evaluating objective and constraints
          m->arg[0] = d->z_cand;
          m->arg[1] = d_nlp->p;
          m->res[0] = &fk_cand;
          m->res[1] = d->z_cand + nx_;
          if (calc_function(m, "nlp_fg")) {
            // line-search failed, skip iteration
            t = beta_ * t;
            continue;
          }

          // Calculating merit-function in candidate
          l1_infeas = casadi_max_viol(nx_+ng_, d->z_cand, d_nlp->lbz, d_nlp->ubz);
          L1merit_cand = fk_cand + m->sigma * l1_infeas;
          if (L1merit_cand <= meritmax + t * c1_ * L1dir) {
            break;
          }

          // Line-search not successful, but we accept it.
          if (ls_iter == max_iter_ls_) {
            ls_success = false;
            break;
          }

          // Backtracking
          t = beta_ * t;
        }

        // Candidate accepted, update dual variables
        casadi_scal(nx_ + ng_, 1-t, d_nlp->lam);
        casadi_axpy(nx_ + ng_, t, d->dlam, d_nlp->lam);

        casadi_scal(nx_, t, d->dx);

      } else {
        // Full step
        casadi_copy(d->dlam, nx_ + ng_, d_nlp->lam);
      }

      // Take step
      casadi_axpy(nx_, 1., d->dx, d_nlp->z);

      if (!exact_hessian_) {
        // Evaluate the gradient of the Lagrangian with the old x but new lam (for BFGS)
        casadi_copy(d->gf, nx_, d->gLag_old);
        casadi_mv(d->Jk, Asp_, d_nlp->lam+nx_, d->gLag_old, true);
        casadi_axpy(nx_, 1., d_nlp->lam, d->gLag_old);
      }
    }

    return 0;
  }

  void Sqpmethod::print_iteration() const {
    print("%4s %14s %9s %9s %9s %7s %2s\n", "iter", "objective", "inf_pr",
          "inf_du", "||d||", "lg(rg)", "ls");
  }

  void Sqpmethod::print_iteration(casadi_int iter, double obj,
                                  double pr_inf, double du_inf,
                                  double dx_norm, double rg,
                                  casadi_int ls_trials, bool ls_success) const {
    print("%4d %14.6e %9.2e %9.2e %9.2e ", iter, obj, pr_inf, du_inf, dx_norm);
    if (rg>0) {
      print("%7.2d ", log10(rg));
    } else {
      print("%7s ", "-");
    }
    print("%2d", ls_trials);
    if (!ls_success) print("F");
    print("\n");
  }

  void Sqpmethod::solve_QP(SqpmethodMemory* m, const double* H, const double* g,
                           const double* lbdz, const double* ubdz, const double* A,
                           double* x_opt, double* dlam) const {
    // Inputs
    fill_n(m->arg, qpsol_.n_in(), nullptr);
    m->arg[CONIC_H] = H;
    m->arg[CONIC_G] = g;
    m->arg[CONIC_X0] = x_opt;
    m->arg[CONIC_LAM_X0] = dlam;
    m->arg[CONIC_LAM_A0] = dlam + nx_;
    m->arg[CONIC_LBX] = lbdz;
    m->arg[CONIC_UBX] = ubdz;
    m->arg[CONIC_A] = A;
    m->arg[CONIC_LBA] = lbdz+nx_;
    m->arg[CONIC_UBA] = ubdz+nx_;

    // Outputs
    fill_n(m->res, qpsol_.n_out(), nullptr);
    m->res[CONIC_X] = x_opt;
    m->res[CONIC_LAM_X] = dlam;
    m->res[CONIC_LAM_A] = dlam + nx_;

    // Solve the QP
    qpsol_(m->arg, m->res, m->iw, m->w, 0);
    if (verbose_) print("QP solved\n");
  }

void Sqpmethod::codegen_declarations(CodeGenerator& g) const {
    if (max_iter_ls_) g.add_dependency(get_function("nlp_fg"));
    g.add_dependency(get_function("nlp_jac_fg"));
    if (exact_hessian_) g.add_dependency(get_function("nlp_hess_l"));
    if (calc_f_ || calc_g_ || calc_lam_x_ || calc_lam_p_)
      g.add_dependency(get_function("nlp_grad"));
    g.add_dependency(qpsol_);
  }

  void Sqpmethod::codegen_body(CodeGenerator& g) const {
    g.add_auxiliary(CodeGenerator::AUX_SQPMETHOD);
    Nlpsol::codegen_body(g);
    // From nlpsol
    g.local("m_p", "const casadi_real", "*");
    g.init_local("m_p", "arg[" + str(NLPSOL_P) + "]");
    g.local("m_f", "casadi_real");
    g << g.copy("arg[" + str(NLPSOL_X0) + "]", nx_, "d_nlp.z") << "\n";
    g << g.copy("arg[" + str(NLPSOL_LAM_X0) + "]", nx_, "d_nlp.lam") << "\n";
    g << g.copy("arg[" + str(NLPSOL_LAM_G0) + "]", ng_, "d_nlp.lam+"+str(nx_)) << "\n";
    g << g.copy("arg[" + str(NLPSOL_LBX) + "]", nx_, "d_nlp.lbz") << "\n";
    g << g.copy("arg[" + str(NLPSOL_LBG) + "]", ng_, "d_nlp.lbz+"+str(nx_)) << "\n";
    g << g.copy("arg[" + str(NLPSOL_UBX) + "]", nx_, "d_nlp.ubz") << "\n";
    g << g.copy("arg[" + str(NLPSOL_UBG) + "]", ng_, "d_nlp.ubz+"+str(nx_)) << "\n";
    casadi_assert(exact_hessian_, "Codegen implemented for exact Hessian only.");

    g.local("d", "struct casadi_sqpmethod_data");
    g.local("p", "struct casadi_sqpmethod_prob");

    g << "d.prob = &p;\n";

    g << "p.sp_h = " << g.sparsity(Hsp_) << ";\n";
    g << "p.sp_a = " << g.sparsity(Asp_) << ";\n";
    g << "p.merit_memsize = " << merit_memsize_ << ";\n";
    g << "p.max_iter_ls = " << max_iter_ls_ << ";\n";
    g << "p.nlp = &p_nlp;\n";
    g << "casadi_sqpmethod_init(&d, &iw, &w);\n";

    g.local("m_w", "casadi_real", "*");
    g << "m_w = w;\n";
    g.local("m_iw", "casadi_int", "*");
    g << "m_iw = iw;\n";
    g.local("m_arg", "const casadi_real", "**");
    g.init_local("m_arg", "arg+" + str(NLPSOL_NUM_IN));
    g.local("m_res", "casadi_real", "**");
    g.init_local("m_res", "res+" + str(NLPSOL_NUM_OUT));
    g.local("iter_count", "casadi_int");
    g.init_local("iter_count", "0");
    if (regularize_) {
      g.local("reg", "casadi_real");
      g.init_local("reg", "0");
    }
    if (max_iter_ls_) {
      g.local("merit_ind", "casadi_int");
      g.init_local("merit_ind", "0");
      g.local("sigma", "casadi_real");
      g.init_local("sigma", "0.0");
      g.local("ls_iter", "casadi_int");
      g.init_local("ls_iter", "0");
      //g.local("ls_success", "casadi_int");
      //g.init_local("ls_success", "1");
      g.local("t", "casadi_real");
      g.init_local("t", "0.0");
    }
    g.local("one", "const casadi_real");
    g.init_local("one", "1");
    g << g.fill("d.dx", nx_, "0.0") << "\n";
    g.comment("MAIN OPTIMIZATION LOOP");
    g << "while (1) {\n";
    g.comment("Evaluate f, g and first order derivative information");
    g << "m_arg[0] = d_nlp.z;\n";
    g << "m_arg[1] = m_p;\n";
    g << "m_res[0] = &m_f;\n";
    g << "m_res[1] = d.gf;\n";
    g << "m_res[2] = d_nlp.z+" + str(nx_) + ";\n";
    g << "m_res[3] = d.Jk;\n";
    std::string nlp_jac_fg = g.add_dependency(get_function("nlp_jac_fg"));
    g << nlp_jac_fg + "(m_arg, m_res, m_iw, m_w, 0);\n";
    g.comment("Evaluate the gradient of the Lagrangian");
    g << g.copy("d.gf", nx_, "d.gLag") << "\n";
    g << g.mv("d.Jk", Asp_, "d_nlp.lam+"+str(nx_), "d.gLag", true) << "\n";
    g << g.axpy(nx_, "1.0", "d_nlp.lam", "d.gLag") << "\n";
    g.comment("Primal infeasability");
    g.local("pr_inf", "casadi_real");
    g << "pr_inf = " << g.max_viol(nx_+ng_, "d_nlp.z", "d_nlp.lbz", "d_nlp.ubz") << ";\n";
    g.comment("inf-norm of lagrange gradient");
    g.local("du_inf", "casadi_real");
    g << "du_inf = " << g.norm_inf(nx_, "d.gLag") << ";\n";
    g.comment("inf-norm of step");
    g.local("dx_norminf", "casadi_real");
    g << "dx_norminf = " << g.norm_inf(nx_, "d.dx") << ";\n";
    g.comment("Checking convergence criteria");
    g << "if (iter_count >= " << min_iter_ << " && pr_inf < " << tol_pr_ <<
          " && du_inf < " << tol_du_ << ") break;\n";
    g << "if (iter_count >= " << max_iter_ << ") break;\n";
    g << "if (iter_count >= 1 && iter_count >= " << min_iter_ << " && dx_norminf <= " <<
          min_step_size_ << ") break;\n";
    g.comment("Update/reset exact Hessian");
    g << "m_arg[0] = d_nlp.z;\n";
    g << "m_arg[1] = m_p;\n";
    g << "m_arg[2] = &one;\n";
    g << "m_arg[3] = d_nlp.lam+" + str(nx_) + ";\n";
    g << "m_res[0] = d.Bk;\n";
    std::string nlp_hess_l = g.add_dependency(get_function("nlp_hess_l"));
    g << nlp_hess_l + "(m_arg, m_res, m_iw, m_w, 0);\n";
    g.comment("Determing regularization parameter with Gershgorin theorem");
    if (regularize_) {
      g << "reg = " << g.fmin("0", "-" + g.lb_eig(Hsp_, "d.Bk")) << "\n";
      g << "if (reg>0) " << g.regularize(Hsp_, "d.Bk", "reg") << "\n";
    }
    g.comment("Formulate the QP");
    g << g.copy("d_nlp.lbz", nx_+ng_, "d.lbdz") << "\n";
    g << g.axpy(nx_+ng_, "-1.0", "d_nlp.z", "d.lbdz") << "\n";
    g << g.copy("d_nlp.ubz", nx_+ng_, "d.ubdz") << "\n";
    g << g.axpy(nx_+ng_, "-1.0", "d_nlp.z", "d.ubdz") << "\n";
    g.comment("Initial guess");
    g << g.copy("d_nlp.lam", nx_+ng_, "d.dlam") << "\n";
    g << g.fill("d.dx", nx_, "0.0") << "\n";
    g.comment("Increase counter");
    g << "iter_count++;\n";
    g.comment("Solve the QP");
    codegen_qp_solve(g, "d.Bk", "d.gf", "d.lbdz", "d.ubdz", "d.Jk", "d.dx", "d.dlam");
    if (max_iter_ls_) {
      g.comment("Detecting indefiniteness");
      g.comment("Calculate penalty parameter of merit function");
      g << "sigma = " << g.fmax("sigma", "(1.01*" + g.norm_inf(nx_+ng_, "d.dlam")+")") << "\n";
      g.comment("Calculate L1-merit function in the actual iterate");
      g.local("l1_infeas", "casadi_real");
      g << "l1_infeas = " << g.max_viol(nx_+ng_, "d_nlp.z", "d_nlp.lbz", "d_nlp.ubz") << ";\n";
      g.comment("Right-hand side of Armijo condition");
      g.local("F_sens", "casadi_real");
      g << "F_sens = " << g.dot(nx_, "d.dx", "d.gf") << ";\n";
      g.local("L1dir", "casadi_real");
      g << "L1dir = F_sens - sigma * l1_infeas;\n";
      g.local("L1merit", "casadi_real");
      g << "L1merit = m_f + sigma * l1_infeas;\n";
      g.comment("Storing the actual merit function value in a list");
      g << "d.merit_mem[merit_ind] = L1merit;\n";
      g << "merit_ind++;\n";
      g << "merit_ind %= " << merit_memsize_ << ";\n";
      g.comment("Calculating maximal merit function value so far");
      g.local("meritmax", "casadi_real");
      g << "meritmax = " << g.vfmax("d.merit_mem+1", g.min(str(merit_memsize_),
           "iter_count")+"-1", "d.merit_mem[0]") << "\n";
      g.comment("Stepsize");
      g << "t = 1.0;\n";
      g.local("fk_cand", "casadi_real");
      g.comment("Merit function value in candidate");
      g.local("L1merit_cand", "casadi_real");
      g << "L1merit_cand = 0.0;\n";
      g.comment("Reset line-search counter, success marker");
      g << "ls_iter = 0;\n";
      //g << "ls_success = 1;\n";
      g.comment("Line-search loop");
      g << "while (1) {\n";
      g.comment(" Increase counter");
      g << "ls_iter++;\n";
      g.comment("Candidate step");
      g << g.copy("d_nlp.z", nx_, "d.z_cand") << "\n";
      g << g.axpy(nx_, "t", "d.dx", "d.z_cand") << "\n";
      g.comment("Evaluating objective and constraints");
      g << "m_arg[0] = d.z_cand;\n;";
      g << "m_arg[1] = m_p;\n;";
      g << "m_res[0] = &fk_cand;\n;";
      g << "m_res[1] = d.z_cand+" + str(nx_) + ";\n;";
      std::string nlp_fg = g.add_dependency(get_function("nlp_fg"));
      g << "if (" << nlp_fg << "(m_arg, m_res, m_iw, m_w, 0)) {\n";
      g.comment("line-search failed, skip iteration");
      g << " t = " << beta_ << "* t;\n";
      g << "continue;\n";
      g << "}\n";
      g.comment("Calculating merit-function in candidate");
      g << "l1_infeas = " << g.max_viol(nx_+ng_, "d.z_cand", "d_nlp.lbz", "d_nlp.ubz") << ";\n";
      g << "L1merit_cand = fk_cand + sigma * l1_infeas;\n";
      g << "if (L1merit_cand <= meritmax + t * " << c1_ << "* L1dir) {\n";
      g << "break;\n";
      g << "}\n";
      g.comment("Line-search not successful, but we accept it.");
      g << "if (ls_iter == " << max_iter_ls_ << ") {\n";
      //g << "ls_success = 0;\n";
      g << "break;\n";
      g << "}\n";
      g.comment("Backtracking");
      g << "t = " << beta_ << "* t;\n";
      g << "}\n";
      g.comment("Candidate accepted, update dual variables");
      g << g.scal(nx_+ng_, "1-t", "d_nlp.lam") << "\n";
      g << g.axpy(nx_+ng_, "t", "d.dlam", "d_nlp.lam") << "\n";
      g << g.scal(nx_, "t", "d.dx") << "\n";
    } else {
      g.comment("Full step");
      g << g.copy("d.dlam", nx_ + ng_, "d_nlp.lam") << "\n";
    }
    g.comment("Take step");
    g << g.axpy(nx_, "1.0", "d.dx", "d_nlp.z") << "\n";
    g << "}\n";
    if (calc_f_ || calc_g_ || calc_lam_x_ || calc_lam_p_) {
      g << "m_arg[0] = d_nlp.z;\n";
      g << "m_arg[1] = m_p;\n";
      g << "m_arg[2] = &one;\n";
      g << "m_arg[3] = d_nlp.lam+" + str(nx_) + ";\n";
      g << "m_res[0] = " << (calc_f_ ? "&m_f" : "0") << ";\n";
      g << "m_res[1] = " << (calc_g_ ? "d_nlp.z+" + str(nx_) : "0") << ";\n";
      g << "m_res[2] = " << (calc_lam_x_ ? "d_nlp.lam+" + str(nx_) : "0") << ";\n";
      g << "m_res[3] = " << (calc_lam_p_ ? "d_nlp.lam_p" : "0") << ";\n";
      std::string nlp_grad = g.add_dependency(get_function("nlp_grad"));
      g << nlp_grad << "(m_arg, m_res, m_iw, m_w, 0);\n";
      if (calc_lam_x_) g << g.scal(nx_, "-1.0", "d_nlp.lam") << "\n";
      if (calc_lam_p_) g << g.scal(np_, "-1.0", "d_nlp.lam_p") << "\n";
    }
    if (bound_consistency_) {
      g << g.bound_consistency(nx_+ng_, "d_nlp.z", "d_nlp.lam", "d_nlp.lbz", "d_nlp.ubz") << ";\n";
    }
    g << g.copy("d_nlp.z", nx_, "res[" + str(NLPSOL_X) + "]") << "\n";
    g << g.copy("d_nlp.z+" + str(nx_), ng_, "res[" + str(NLPSOL_G) + "]") << "\n";
    g << g.copy("d_nlp.lam", nx_, "res[" + str(NLPSOL_LAM_X) + "]") << "\n";
    g << g.copy("d_nlp.lam+"+str(nx_), ng_, "res[" + str(NLPSOL_LAM_G) + "]") << "\n";
    g << g.copy("d_nlp.lam_p", np_, "res[" + str(NLPSOL_LAM_P) + "]") << "\n";
    g << g.copy("&m_f", 1, "res[" + str(NLPSOL_F) + "]") << "\n";
  }
  void Sqpmethod::codegen_qp_solve(CodeGenerator& cg, const std::string&  H, const std::string& g,
              const std::string&  lbdz, const std::string& ubdz,
              const std::string&  A, const std::string& x_opt, const std::string&  dlam) const {
    for (casadi_int i=0;i<qpsol_.n_in();++i) cg << "m_arg[" << i << "] = 0;\n";
    cg << "m_arg[" << CONIC_H << "] = " << H << ";\n";
    cg << "m_arg[" << CONIC_G << "] = " << g << ";\n";
    cg << "m_arg[" << CONIC_X0 << "] = " << x_opt << ";\n";
    cg << "m_arg[" << CONIC_LAM_X0 << "] = " << dlam << ";\n";
    cg << "m_arg[" << CONIC_LAM_A0 << "] = " << dlam << "+" << nx_ << ";\n";
    cg << "m_arg[" << CONIC_LBX << "] = " << lbdz << ";\n";
    cg << "m_arg[" << CONIC_UBX << "] = " << ubdz << ";\n";
    cg << "m_arg[" << CONIC_A << "] = " << A << ";\n";
    cg << "m_arg[" << CONIC_LBA << "] = " << lbdz << "+" << nx_ << ";\n";
    cg << "m_arg[" << CONIC_UBA << "] = " << ubdz << "+" << nx_ << ";\n";
    for (casadi_int i=0;i<qpsol_.n_out();++i) cg << "m_res[" << i << "] = 0;\n";
    cg << "m_res[" << CONIC_X << "] = " << x_opt << ";\n";
    cg << "m_res[" << CONIC_LAM_X << "] = " << dlam << ";\n";
    cg << "m_res[" << CONIC_LAM_A << "] = " << dlam << "+" << nx_ << ";\n";
    std::string qpsol = cg.add_dependency(qpsol_);
    cg << qpsol << "(m_arg, m_res, m_iw, m_w, 0);\n";
  }

  Dict Sqpmethod::get_stats(void* mem) const {
    Dict stats = Nlpsol::get_stats(mem);
    auto m = static_cast<SqpmethodMemory*>(mem);
    stats["return_status"] = m->return_status;
    stats["iter_count"] = m->iter_count;
    return stats;
  }

  Sqpmethod::Sqpmethod(DeserializingStream& s) : Nlpsol(s) {
    s.version("Sqpmethod", 1);
    s.unpack("Sqpmethod::qpsol", qpsol_);
    s.unpack("Sqpmethod::exact_hessian", exact_hessian_);
    s.unpack("Sqpmethod::max_iter", max_iter_);
    s.unpack("Sqpmethod::min_iter", min_iter_);
    s.unpack("Sqpmethod::lbfgs_memory", lbfgs_memory_);
    s.unpack("Sqpmethod::tol_pr_", tol_pr_);
    s.unpack("Sqpmethod::tol_du_", tol_du_);
    s.unpack("Sqpmethod::min_step_size_", min_step_size_);
    s.unpack("Sqpmethod::c1", c1_);
    s.unpack("Sqpmethod::beta", beta_);
    s.unpack("Sqpmethod::max_iter_ls_", max_iter_ls_);
    s.unpack("Sqpmethod::merit_memsize_", merit_memsize_);
    s.unpack("Sqpmethod::beta", beta_);
    s.unpack("Sqpmethod::print_header", print_header_);
    s.unpack("Sqpmethod::print_iteration", print_iteration_);
    s.unpack("Sqpmethod::print_status", print_status_);
    s.unpack("Sqpmethod::Hsp", Hsp_);
    s.unpack("Sqpmethod::Asp", Asp_);
    s.unpack("Sqpmethod::regularize", regularize_);
    set_sqpmethod_prob();
  }

  void Sqpmethod::serialize_body(SerializingStream &s) const {
    Nlpsol::serialize_body(s);
    s.version("Sqpmethod", 1);
    s.pack("Sqpmethod::qpsol", qpsol_);
    s.pack("Sqpmethod::exact_hessian", exact_hessian_);
    s.pack("Sqpmethod::max_iter", max_iter_);
    s.pack("Sqpmethod::min_iter", min_iter_);
    s.pack("Sqpmethod::lbfgs_memory", lbfgs_memory_);
    s.pack("Sqpmethod::tol_pr_", tol_pr_);
    s.pack("Sqpmethod::tol_du_", tol_du_);
    s.pack("Sqpmethod::min_step_size_", min_step_size_);
    s.pack("Sqpmethod::c1", c1_);
    s.pack("Sqpmethod::beta", beta_);
    s.pack("Sqpmethod::max_iter_ls_", max_iter_ls_);
    s.pack("Sqpmethod::merit_memsize_", merit_memsize_);
    s.pack("Sqpmethod::beta", beta_);
    s.pack("Sqpmethod::print_header", print_header_);
    s.pack("Sqpmethod::print_iteration", print_iteration_);
    s.pack("Sqpmethod::print_status", print_status_);
    s.pack("Sqpmethod::Hsp", Hsp_);
    s.pack("Sqpmethod::Asp", Asp_);
    s.pack("Sqpmethod::regularize", regularize_);
  }
} // namespace casadi
