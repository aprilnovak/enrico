#include "enrico/nek_driver.h"

#include "enrico/error.h"
#include "nek5000/core/nek_interface.h"

#include <gsl/gsl>

#include <climits>
#include <fstream>
#include <string>
#include <unistd.h>

namespace enrico {

NekDriver::NekDriver(MPI_Comm comm, pugi::xml_node node)
  : Driver(comm)
{
  pressure_ = node.child("pressure").text().as_double();
  lelg_ = nek_get_lelg();
  lelt_ = nek_get_lelt();
  lx1_ = nek_get_lx1();

  Expects(pressure_ > 0.0);

  if (active()) {
    casename_ = node.child_value("casename");
    if (comm_.rank == 0) {
      init_session_name();
    }

    MPI_Fint int_comm = MPI_Comm_c2f(comm_.comm);
    C2F_nek_init(static_cast<const int*>(&int_comm));

    nelgt_ = nek_get_nelgt();
    nelt_ = nek_get_nelt();

    init_displs();
  }
  MPI_Barrier(MPI_COMM_WORLD);
}

// This version sets the local-to-global element ordering, as ensured by a Gatherv
// operation. It is currently unused, as the coupling does not need to know the
// local-global ordering. void NekDriver::init_mappings() {
//   if(active()) {
//
//     // These arrays are only gathered on the root process
//     if (comm_.rank == 0) {
//       local_counts_.reserve(comm_.size);
//       local_displs_.reserve(comm_.size);
//       local_ordering_.reserve(nelgt_);
//     }
//
//     // Every proc sets a mapping from its local to global element indices.
//     // This mapping is in a Nek5000 common block, but we don't expose it to C++
//     int local_to_global[nelt_];
//     for (int i = 0; i < nelt_; ++i) {
//       local_to_global[i] = nek_get_global_elem(i+1) - 1;
//     }
//
//     // The root proc gets every proc's local element count.
//     comm_.Gather(&nelt_, 1, MPI_INT, local_counts_.data(), 1, MPI_INT);
//
//     if (comm_.rank == 0) {
//       // The root makes a list of data displacements for a Gatherv operation.
//       // Each proc's data will be displaced by the number of local elements on
//       // the previous proc.
//       local_displs_.at(0) = 0;
//       for (int i = 1; i < comm_.rank; ++i) {
//         local_displs_.at(i) = local_displs_.at(i-1) + local_counts_.at(i-1);
//       }
//
//       // The root the gets the local-to-global element mapping for all procs.
//       // This can be used to reorder the data from a Gatherv operation.
//       comm_.Gatherv(
//           local_to_global, nelt_, MPI_INT,
//           local_ordering_.data(), local_counts_.data(), local_displs_.data(), MPI_INT
//       );
//     }
//     else {
//       // Other procs send their local-to-global mapping to the root.
//       comm_.Gatherv(
//           local_to_global, nelt_, MPI_INT,
//           nullptr, nullptr, nullptr, MPI_INT
//       );
//     }
//   }
// }

void NekDriver::init_session_name()
{
  char path_buffer[PATH_MAX];
  err_chk(getcwd(path_buffer, PATH_MAX) == path_buffer ? 0 : -1,
          "Error writing SESSION.NAME in NekDriver");

  std::ofstream session_name("SESSION.NAME");
  session_name << casename_ << std::endl << path_buffer << std::endl;
  session_name.close();
}

void NekDriver::init_displs()
{
  if (active()) {
    local_counts_.resize(comm_.size);
    local_displs_.resize(comm_.size);

    comm_.Allgather(&nelt_, 1, MPI_INT, local_counts_.data(), 1, MPI_INT);

    local_displs_.at(0) = 0;
    for (int i = 1; i < comm_.size; ++i) {
      local_displs_.at(i) = local_displs_.at(i - 1) + local_counts_.at(i - 1);
    }
  }
}

xt::xtensor<double, 1> NekDriver::temperature() const
{
  // Each Nek proc finds the temperatures of its local elements
  double local_elem_temperatures[nelt_];
  for (int i = 0; i < nelt_; ++i) {
    local_elem_temperatures[i] = get_local_elem_temperature(i + 1);
  }

  xt::xtensor<double, 1> global_elem_temperatures = xt::xtensor<double, 1>();

  // only the rank 0 process allocates the size for the receive buffer
  if (comm_.rank == 0) {
    global_elem_temperatures.resize({gsl::narrow<std::size_t>(nelgt_)});
  }

  // Gather all the local element temperatures onto the root
  comm_.Gatherv(local_elem_temperatures,
                nelt_,
                MPI_DOUBLE,
                global_elem_temperatures.data(),
                local_counts_.data(),
                local_displs_.data(),
                MPI_DOUBLE);

  // only the return value from root should be used, or else a broadcast added here
  return global_elem_temperatures;
}

void NekDriver::solve_step()
{
  nek_reset_counters();
  C2F_nek_solve();
}

Position NekDriver::get_global_elem_centroid(int global_elem) const
{
  double x, y, z;
  err_chk(nek_get_global_elem_centroid(global_elem, &x, &y, &z),
          "Could not find centroid of global element " + std::to_string(global_elem));
  return {x, y, z};
}

Position NekDriver::get_local_elem_centroid(int local_elem) const
{
  double x, y, z;
  err_chk(nek_get_local_elem_centroid(local_elem, &x, &y, &z),
          "Could not find centroid of local element " + std::to_string(local_elem));
  return {x, y, z};
}

double NekDriver::get_local_elem_volume(int local_elem) const
{
  double volume;
  err_chk(nek_get_local_elem_volume(local_elem, &volume),
          "Could not find volume of local element " + std::to_string(local_elem));
  return volume;
}

double NekDriver::get_local_elem_temperature(int local_elem) const
{
  double temperature;
  err_chk(nek_get_local_elem_temperature(local_elem, &temperature),
          "Could not find temperature of local element " + std::to_string(local_elem));
  return temperature;
}

bool NekDriver::global_elem_is_in_rank(int global_elem) const
{
  return (nek_global_elem_is_in_rank(global_elem, comm_.rank) == 1);
}

int NekDriver::global_elem_is_in_fluid(int global_elem) const
{
  return nek_global_elem_is_in_fluid(global_elem);
}

int NekDriver::local_elem_is_in_fluid(int local_elem) const
{
  return nek_local_elem_is_in_fluid(local_elem);
}

int NekDriver::set_heat_source(int local_elem, double heat) const
{
  return nek_set_heat_source(local_elem, heat);
}

NekDriver::~NekDriver()
{
  if (active())
    C2F_nek_end();
  MPI_Barrier(MPI_COMM_WORLD);
}

} // namespace enrico
