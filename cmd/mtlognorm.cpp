/* Copyright (c) 2008-2017 the MRtrix3 contributors.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, you can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * MRtrix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * For more details, see http://www.mrtrix.org/.
 */


#include "command.h"
#include "image.h"
#include "algo/loop.h"
#include "adapter/extract.h"
#include "filter/optimal_threshold.h"
#include "filter/mask_clean.h"
#include "filter/connected_components.h"
#include "transform.h"
#include "math/least_squares.h"
#include "algo/threaded_copy.h"

using namespace MR;
using namespace App;

#define DEFAULT_NORM_VALUE 0.282094
#define DEFAULT_MAXITER_VALUE 10

void usage ()
{
  AUTHOR = "Thijs Dhollander (thijs.dhollander@gmail.com), Rami Tabbara (rami.tabbara@florey.edu.au) and David Raffelt (david.raffelt@florey.edu.au)";

  SYNOPSIS = "Multi-tissue informed log-domain intensity normalisation";

  DESCRIPTION
   + "This command inputs N number of tissue components "
     "(e.g. from multi-tissue CSD), and outputs N corrected tissue components. Intensity normalisation is performed by either "
     "determining a common global normalisation factor for all tissue types (default) or by normalising each tissue type independently "
     "with a single tissue-specific global scale factor."

   + "The -mask option is mandatory, and is optimally provided with a brain mask, such as the one obtained from dwi2mask earlier in the processing pipeline."

   + "Example usage: mtlognorm wm.mif wm_norm.mif gm.mif gm_norm.mif csf.mif csf_norm.mif -mask mask.mif.";

  ARGUMENTS
    + Argument ("input output", "list of all input and output tissue compartment files. See example usage in the description. "
                              "Note that any number of tissues can be normalised").type_image_in().allow_multiple();

  OPTIONS
    + Option ("mask", "define the mask to compute the normalisation within. This option is mandatory.").required ()
    + Argument ("image").type_image_in ()

    + Option ("value", "specify the value to which the summed tissue compartments will be normalised to "
                       "(Default: sqrt(1/(4*pi)) = " + str(DEFAULT_NORM_VALUE, 6) + ")")
    + Argument ("number").type_float ()

    + Option ("bias", "output the estimated bias field")
    + Argument ("image").type_image_out ()

    + Option ("independent", "intensity normalise each tissue type independently")

    + Option ("maxiter", "set the number of iterations. Default(" + str(DEFAULT_MAXITER_VALUE) + ").")
    + Argument ("number").type_integer()

    + Option ("check", "check the final mask used to compute the bias field. This mask excludes outlier regions ignored by the bias field fitting procedure. However, these regions are still corrected for bias fields based on the other image data.")
    + Argument ("image").type_image_out ();
}

const int n_basis_vecs (20);


FORCE_INLINE Eigen::MatrixXd basis_function (const Eigen::Vector3 pos) {
  double x = pos[0];
  double y = pos[1];
  double z = pos[2];
  Eigen::MatrixXd basis(n_basis_vecs, 1);
  basis(0) = 1.0;
  basis(1) = x;
  basis(2) = y;
  basis(3) = z;
  basis(4) = x * x;
  basis(5) = y * y;
  basis(6) = z * z;
  basis(7) = x * y;
  basis(8) = x * z;
  basis(9) = y * z;
  basis(10) = x * x * x;
  basis(11) = y * y * y;
  basis(12) = z * z * z;
  basis(13) = x * x * y;
  basis(14) = x * x * z;
  basis(15) = y * y * x;
  basis(16) = y * y * z;
  basis(17) = z * z * x;
  basis(18) = z * z * y;
  basis(19) = x * y * z;
  return basis;
}

FORCE_INLINE void refine_mask (Image<float>& summed,
  Image<bool>& initial_mask,
  Image<bool>& refined_mask) {

  for (auto i = Loop (summed, 0, 3) (summed, initial_mask, refined_mask); i; ++i) {
    if (std::isfinite((float) summed.value ()) && summed.value () > 0.f && initial_mask.value ())
      refined_mask.value () = true;
    else
      refined_mask.value () = false;
  }
}


void run ()
{
  if (argument.size() % 2)
    throw Exception ("The number of input arguments must be even. There must be an output file provided for every input tissue image");

  if (argument.size() < 4)
    throw Exception ("At least two tissue types must be provided");

  ProgressBar progress ("performing intensity normalisation and bias field correction...");
  vector<Image<float> > input_images;
  vector<Header> output_headers;
  vector<std::string> output_filenames;


  // Open input images and prepare output image headers
  for (size_t i = 0; i < argument.size(); i += 2) {
    progress++;
    input_images.emplace_back (Image<float>::open (argument[i]));

    if (i > 0)
      check_dimensions (input_images[0], input_images[i / 2], 0, 3);

    if (Path::exists (argument[i + 1]) && !App::overwrite_files)
      throw Exception ("output file \"" + argument[i] + "\" already exists (use -force option to force overwrite)");

    output_headers.emplace_back (Header::open (argument[i]));
    output_filenames.emplace_back (argument[i + 1]);
  }

  const size_t n_tissue_types = input_images.size();


  // Load the mask and refine the initial mask to exclude non-positive summed tissue components
  Header header_3D (input_images[0]);
  header_3D.ndim() = 3;
  auto opt = get_options ("mask");

  auto orig_mask = Image<bool>::open (opt[0][0]);
  auto initial_mask = Image<bool>::scratch (orig_mask);
  auto mask = Image<bool>::scratch (orig_mask);

  auto summed = Image<float>::scratch (header_3D);
  for (size_t j = 0; j < input_images.size(); ++j) {
    for (auto i = Loop (summed, 0, 3) (summed, input_images[j]); i; ++i)
      summed.value() += input_images[j].value();
    progress++;
  }

  refine_mask (summed, orig_mask, initial_mask);

  threaded_copy (initial_mask, mask);


  // Load input images into single 4d-image and zero-clamp combined-tissue image
  Header h_combined_tissue (input_images[0]);
  h_combined_tissue.ndim () = 4;
  h_combined_tissue.size (3) = n_tissue_types;
  auto combined_tissue = Image<float>::scratch (h_combined_tissue, "Packed tissue components");

  for (size_t i = 0; i < n_tissue_types; ++i) {
    combined_tissue.index (3) = i;
    for (auto l = Loop (0, 3) (combined_tissue, input_images[i]); l; ++l) {
      combined_tissue.value () = std::max<float>(input_images[i].value (), 0.f);
    }
  }

  size_t num_voxels = 0;
  for (auto i = Loop (mask) (mask); i; ++i) {
    if (mask.value())
      num_voxels++;
  }

  if (!num_voxels)
    throw Exception ("Error in automatic mask generation. Mask contains no voxels");


  // Load global normalisation factor
  const float normalisation_value = get_option_value ("value", DEFAULT_NORM_VALUE);

  if (normalisation_value <= 0.f)
    throw Exception ("Intensity normalisation value must be strictly positive.");

  const float log_norm_value = std::log (normalisation_value);

  const size_t max_iter = get_option_value ("maxiter", DEFAULT_MAXITER_VALUE);


  // Initialise bias fields in both image and log domain
  Eigen::MatrixXd bias_field_weights (n_basis_vecs, 0);

  auto bias_field_image = Image<float>::scratch (header_3D);
  auto bias_field_log = Image<float>::scratch (header_3D);

  for (auto i = Loop(bias_field_log) (bias_field_image, bias_field_log); i; ++i) {
    bias_field_image.value() = 1.f;
    bias_field_log.value() = 0.f;
  }

  Eigen::VectorXd scale_factors (n_tissue_types);
  Eigen::VectorXd previous_scale_factors (n_tissue_types);

  size_t iter = 1;


  while (iter < max_iter) {

    INFO ("iteration: " + str(iter));

    // Iteratively compute intensity normalisation scale factors
    // with outlier rejection
    size_t norm_iter = 1;
    bool norm_converged = false;

    while (!norm_converged && norm_iter < max_iter) {

      INFO ("norm iteration: " + str(norm_iter));

      // Solve for tissue normalisation scale factors
      Eigen::MatrixXd X (num_voxels, n_tissue_types);
      Eigen::VectorXd y (num_voxels);
      y.fill (1);
      uint32_t index = 0;

      for (auto i = Loop (mask) (mask, combined_tissue, bias_field_image); i; ++i) {
        if (mask.value()) {
          for (size_t j = 0; j < n_tissue_types; ++j) {
            combined_tissue.index (3) = j;
            X (index, j) = combined_tissue.value() / bias_field_image.value();
          }
          ++index;
        }
      }

      scale_factors = X.colPivHouseholderQr().solve(y);

      // Ensure our scale factors satisfy the condition that sum(log(scale_factors)) = 0
      double log_sum = 0.f;
      for (size_t j = 0; j < n_tissue_types; ++j) {
        if (scale_factors(j) <= 0.0)
          throw Exception ("Non-positive tissue intensity normalisation scale factor was computed."
                           " Tissue index: " + str(j) + " Scale factor: " + str(scale_factors(j)) +
                           " Needs to be strictly positive!");
        log_sum += std::log (scale_factors(j));
      }
      scale_factors /= std::exp (log_sum / n_tissue_types);

      // Check for convergence
      if (iter > 1) {
        Eigen::VectorXd diff = previous_scale_factors.array() - scale_factors.array();
        diff = diff.array().abs() / previous_scale_factors.array();
        INFO ("percentage change in estimated scale factors: " + str(diff.mean() * 100));
        if (diff.mean() < 0.001)
          norm_converged = true;
      }

      // Perform outlier rejection on log-domain of summed images
      if (!norm_converged) {

        auto summed_log = Image<float>::scratch (header_3D);
        for (size_t j = 0; j < n_tissue_types; ++j) {
          for (auto i = Loop (summed_log, 0, 3) (summed_log, combined_tissue, bias_field_image); i; ++i) {
            combined_tissue.index(3) = j;
            summed_log.value() += scale_factors(j) * combined_tissue.value() / bias_field_image.value();
          }

          summed_log.value() = std::log(summed_log.value());
        }

        refine_mask (summed_log, initial_mask, mask);

        vector<float> summed_log_values;
        for (auto i = Loop (mask) (mask, summed_log); i; ++i) {
          if (mask.value())
            summed_log_values.emplace_back (summed_log.value());
        }

        num_voxels = summed_log_values.size();

        std::sort (summed_log_values.begin(), summed_log_values.end());
        float lower_quartile = summed_log_values[std::round ((float)num_voxels * 0.25)];
        float upper_quartile = summed_log_values[std::round ((float)num_voxels * 0.75)];
        float upper_outlier_threshold = upper_quartile + 1.6 * (upper_quartile - lower_quartile);
        float lower_outlier_threshold = lower_quartile - 1.6 * (upper_quartile - lower_quartile);


        for (auto i = Loop (mask) (mask, summed_log); i; ++i) {
          if (mask.value()) {
            if (summed_log.value() < lower_outlier_threshold || summed_log.value() > upper_outlier_threshold) {
              mask.value() = 0;
              num_voxels--;
            }
          }
        }

        if (log_level >= 3)
          display (mask);
      }

      previous_scale_factors = scale_factors;

      norm_iter++;
    }


    INFO ("scale factors: " + str(scale_factors.transpose()));


    // Solve for bias field weights in the log domain
    Transform transform (mask);
    Eigen::MatrixXd bias_field_basis (num_voxels, n_basis_vecs);
    Eigen::MatrixXd X (num_voxels, n_tissue_types);
    Eigen::VectorXd y (num_voxels);
    uint32_t index = 0;
    for (auto i = Loop (mask) (mask, combined_tissue); i; ++i) {
      if (mask.value()) {
        Eigen::Vector3 vox (mask.index(0), mask.index(1), mask.index(2));
        Eigen::Vector3 pos = transform.voxel2scanner * vox;
        bias_field_basis.row (index) = basis_function (pos).col(0);

        double sum = 0.0;
        for (size_t j = 0; j < n_tissue_types; ++j) {
          combined_tissue.index(3) = j;
          sum += scale_factors(j) * combined_tissue.value() ;
        }
        y (index++) = std::log(sum) - log_norm_value;
      }
    }

    bias_field_weights = bias_field_basis.colPivHouseholderQr().solve(y);

    // Generate bias field in the log domain
    for (auto i = Loop (bias_field_log) (bias_field_log); i; ++i) {
        Eigen::Vector3 vox (bias_field_log.index(0), bias_field_log.index(1), bias_field_log.index(2));
        Eigen::Vector3 pos = transform.voxel2scanner * vox;
        bias_field_log.value() = basis_function (pos).col(0).dot (bias_field_weights.col(0));
    }

    // Generate bias field in the image domain
    for (auto i = Loop (bias_field_log) (bias_field_log, bias_field_image); i; ++i)
        bias_field_image.value () = std::exp(bias_field_log.value());

    progress++;
    iter++;
  }

  opt = get_options ("bias");
  if (opt.size()) {
    auto bias_field_output = Image<float>::create (opt[0][0], header_3D);
    threaded_copy (bias_field_image, bias_field_output);
  }
  progress++;

  opt = get_options ("check");
  if (opt.size()) {
    auto mask_output = Image<float>::create (opt[0][0], mask);
    threaded_copy (mask, mask_output);
  }
  progress++;

  // Compute mean of all scale factors in the log domain
  opt = get_options ("independent");
  if (!opt.size()) {
    float mean = 0.0;
    for (int i = 0; i < scale_factors.size(); ++i)
      mean += std::log(scale_factors(i, 0));
    mean /= scale_factors.size();
    mean = std::exp (mean);
    scale_factors.fill (mean);
  }

  // Output bias corrected and normalised tissue maps
  uint32_t total_count = 0;
  for (size_t i = 0; i < output_headers.size(); ++i) {
    uint32_t count = 1;
    for (size_t j = 0; j < output_headers[i].ndim(); ++j)
      count *= output_headers[i].size(j);
    total_count += count;
  }

  for (size_t j = 0; j < output_filenames.size(); ++j) {
    output_headers[j].keyval()["normalisation_scale_factor"] = str(scale_factors(j, 0));
    auto output_image = Image<float>::create (output_filenames[j], output_headers[j]);
    for (auto i = Loop (output_image) (output_image, input_images[j]); i; ++i) {
      assign_pos_of (output_image, 0, 3).to (bias_field_image);
      output_image.value() = scale_factors(j, 0) * input_images[j].value() / bias_field_image.value();
      output_image.value() = std::max<float>(output_image.value(), 0.f);
    }
  }
}