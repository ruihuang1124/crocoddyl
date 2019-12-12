///////////////////////////////////////////////////////////////////////////////
// BSD 3-Clause License
//
// Copyright (C) 2019-2020, University of Edinburgh
// Copyright note valid unless otherwise stated in individual files.
// All rights reserved.
///////////////////////////////////////////////////////////////////////////////

#ifndef CROCODDYL_CORE_SOLVERS_BOX_QP_HXX_
#define CROCODDYL_CORE_SOLVERS_BOX_QP_HXX_

namespace crocoddyl {

BoxQP::BoxQP(const std::size_t nx, std::size_t maxiter, const double th_acceptstep, const double th_grad,
             const double reg)
    : nx_(nx),
      maxiter_(maxiter),
      th_acceptstep_(th_acceptstep),
      th_grad_(th_grad),
      reg_(reg),
      x_(nx),
      xnew_(nx),
      g_(nx),
      dx_(nx) {
  clamped_idx_.reserve(nx_);
  free_idx_.reserve(nx_);
  const std::size_t& n_alphas_ = 10;
  alphas_.resize(n_alphas_);
  for (std::size_t n = 0; n < n_alphas_; ++n) {
    alphas_[n] = 1. / pow(2., static_cast<double>(n));
  }
}

BoxQP::~BoxQP() {}

const BoxQPSolution& BoxQP::solve(const Eigen::MatrixXd& H, const Eigen::VectorXd& q, const Eigen::VectorXd& lb,
                                  const Eigen::VectorXd& ub, const Eigen::VectorXd& xinit) {
  if (static_cast<std::size_t>(H.rows()) != nx_ || static_cast<std::size_t>(H.cols()) != nx_) {
    throw_pretty("Invalid argument: "
                 << "H has wrong dimension (it should be " + std::to_string(nx_) + "," + std::to_string(nx_) + ")");
  }
  if (static_cast<std::size_t>(q.size()) != nx_) {
    throw_pretty("Invalid argument: "
                 << "q has wrong dimension (it should be " + std::to_string(nx_) + ")");
  }
  if (static_cast<std::size_t>(lb.size()) != nx_) {
    throw_pretty("Invalid argument: "
                 << "lb has wrong dimension (it should be " + std::to_string(nx_) + ")");
  }
  if (static_cast<std::size_t>(ub.size()) != nx_) {
    throw_pretty("Invalid argument: "
                 << "ub has wrong dimension (it should be " + std::to_string(nx_) + ")");
  }
  if (static_cast<std::size_t>(xinit.size()) != nx_) {
    throw_pretty("Invalid argument: "
                 << "xinit has wrong dimension (it should be " + std::to_string(nx_) + ")");
  }

  // We need to enforce feasible warm-starting of the algorithm
  for (std::size_t i = 0; i < nx_; ++i) {
    x_(i) = std::max(std::min(xinit(i), ub(i)), lb(i));
  }

  // Start the numerical iterations
  for (std::size_t k = 0; k < maxiter_; ++k) {
    clamped_idx_.clear();
    free_idx_.clear();
    // Compute the gradient
    g_ = q;
    g_.noalias() += H * x_;
    for (std::size_t j = 0; j < nx_; ++j) {
      const double& gj = g_(j);
      const double& xj = x_(j);
      const double& lbj = lb(j);
      const double& ubj = ub(j);
      if ((xj == lbj && gj > 0.) || (xj == ubj && gj < 0.)) {
        clamped_idx_.push_back(j);
      } else {
        free_idx_.push_back(j);
      }
    }

    // Check convergence
    nf_ = free_idx_.size();
    nc_ = clamped_idx_.size();
    if (g_.lpNorm<Eigen::Infinity>() <= th_grad_ || nf_ == 0) {
      if (k == 0) {  // compute the inverse of the free Hessian
        Hff_.resize(nf_, nf_);
        for (std::size_t i = 0; i < nf_; ++i) {
          const std::size_t& fi = free_idx_[i];
          for (std::size_t j = 0; j < nf_; ++j) {
            Hff_(i, j) = H(fi, free_idx_[j]);
          }
        }
        if (reg_ != 0.) {
            Hff_.diagonal().array() += reg_;
        }
        Hff_inv_llt_ = Eigen::LLT<Eigen::MatrixXd>(nf_);
        Hff_inv_llt_.compute(Hff_);
        Eigen::ComputationInfo info = Hff_inv_llt_.info();
        if (info != Eigen::Success) {
          throw_pretty("backward_error");
        }
        Hff_inv_.setIdentity(nf_, nf_);
        Hff_inv_llt_.solveInPlace(Hff_inv_);
      }
      solution_.Hff_inv = Hff_inv_;
      solution_.x = x_;
      solution_.free_idx = free_idx_;
      solution_.clamped_idx = clamped_idx_;
      return solution_;
    }

    // Compute the search direction as Newton step along the free space
    qf_.resize(nf_);
    xf_.resize(nf_);
    xc_.resize(nc_);
    dxf_.resize(nf_);
    Hff_.resize(nf_, nf_);
    Hfc_.resize(nf_, nc_);
    for (std::size_t i = 0; i < nf_; ++i) {
      const std::size_t& fi = free_idx_[i];
      qf_(i) = q(fi);
      xf_(i) = x_(fi);
      for (std::size_t j = 0; j < nf_; ++j) {
        Hff_(i, j) = H(fi, free_idx_[j]);
      }
      for (std::size_t j = 0; j < nc_; ++j) {
        const std::size_t cj = clamped_idx_[j];
        xc_(j) = x_(cj);
        Hfc_(i, j) = H(fi, cj);
      }
    }
    if (reg_ != 0.) {
        Hff_.diagonal().array() += reg_;
    }
    Hff_inv_llt_ = Eigen::LLT<Eigen::MatrixXd>(nf_);
    Hff_inv_llt_.compute(Hff_);
    Eigen::ComputationInfo info = Hff_inv_llt_.info();
    if (info != Eigen::Success) {
      throw_pretty("backward_error");
    }
    Hff_inv_.setIdentity(nf_, nf_);
    Hff_inv_llt_.solveInPlace(Hff_inv_);
    dxf_ = -qf_;
    if (nc_ != 0) {
      dxf_.noalias() -= Hfc_ * xc_;
    }
    Hff_inv_llt_.solveInPlace(dxf_);
    dxf_ -= xf_;
    dx_.setZero();
    for (std::size_t i = 0; i < nf_; ++i) {
      dx_(free_idx_[i]) = dxf_(i);
    }

    // There is not improving anymore
    if (dx_.lpNorm<Eigen::Infinity>() < th_grad_) {
      solution_.Hff_inv = Hff_inv_;
      solution_.x = x_;
      solution_.free_idx = free_idx_;
      solution_.clamped_idx = clamped_idx_;
      return solution_;
    }

    // Try different step lengths
    double fold = 0.5 * x_.dot(H * x_) + q.dot(x_);
    for (std::vector<double>::const_iterator it = alphas_.begin(); it != alphas_.end(); ++it) {
      double steplength = *it;
      for (std::size_t i = 0; i < nx_; ++i) {
        xnew_(i) = std::max(std::min(x_(i) + steplength * dx_(i), ub(i)), lb(i));
      }
      double fnew = 0.5 * xnew_.dot(H * xnew_) + q.dot(xnew_);
      if (fold - fnew > th_acceptstep_ * g_.dot(x_ - xnew_)) {
        x_ = xnew_;
        break;
      }
    }
  }
  solution_.Hff_inv = Hff_inv_;
  solution_.x = x_;
  solution_.free_idx = free_idx_;
  solution_.clamped_idx = clamped_idx_;
  return solution_;
}

}  // namespace crocoddyl

#endif  // CROCODDYL_CORE_SOLVERS_BOX_QP_HXX_
