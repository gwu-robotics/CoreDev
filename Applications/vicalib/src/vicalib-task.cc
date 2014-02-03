// Copyright (c) George Washington University, all rights reserved.  See the
// accompanying LICENSE file for more information.

#include <visual-inertial-calibration/vicalib-task.h>

#include <time.h>

#include <CVars/CVar.h>
#include <calibu/conics/ConicFinder.h>
#include <calibu/pose/Pnp.h>
#include <PbMsgs/Matrix.h>

#ifdef HAVE_PANGOLIN
#include <calibu/gl/Drawing.h>
#include <pangolin/gldraw.h>
#endif  // HAVE_PANGOLIN

DECLARE_bool(has_initial_guess);  // Defined in vicalib-engine.cc.
DEFINE_double(function_tolerance, 1e-6,
              "Convergence criteria for the optimizer.");

DEFINE_double(max_fx_diff, 10.0, "Maximum fx difference between calibrations.");
DEFINE_double(max_fy_diff, 10.0, "Maximum fy difference between calibrations.");
DEFINE_double(max_cx_diff, 10.0, "Maximum cx difference between calibrations.");
DEFINE_double(max_cy_diff, 10.0, "Maximum cy difference between calibrations.");

DEFINE_double(max_fov_w_diff, 0.3,
              "Maximum fov w parameter difference between calibrations.");

DEFINE_double(max_poly3_diff_k1, 0.1,
              "Maximum poly3 k1 difference between calibrations.");
DEFINE_double(max_poly3_diff_k2, 0.1,
              "Maximum poly3 k2 difference between calibrations.");
DEFINE_double(max_poly3_diff_k3, 0.1,
              "Maximum poly3 k3 difference between calibrations.");

namespace visual_inertial_calibration {

struct VicalibGuiOptions {
#ifdef HAVE_PANGOLIN
  VicalibGuiOptions() :
      disp_mse("ui.MSE"),
      disp_frame("ui.frame"),
      disp_thresh("ui.Display Thresh", false),
      disp_lines("ui.Display Lines", true),
      disp_cross("ui.Display crosses", true),
      disp_bbox("ui.Display bbox", true),
      disp_barcode("ui.Display barcode", false) {}

  pangolin::Var<double> disp_mse;
  pangolin::Var<int> disp_frame;
  pangolin::Var<bool> disp_thresh;
  pangolin::Var<bool> disp_lines;
  pangolin::Var<bool> disp_cross;
  pangolin::Var<bool> disp_bbox;
  pangolin::Var<bool> disp_barcode;
#endif  // HAVE_PANGOLIN
};

VicalibTask::VicalibTask(
        size_t num_streams,
        const std::vector<size_t>& width,
        const std::vector<size_t>& height,
        int grid_seed,
        double grid_spacing,
        const Eigen::Vector2i& grid_size,
        bool fix_intrinsics,
        const std::vector<CameraAndPose,
        Eigen::aligned_allocator<CameraAndPose> >& input_cameras,
        const std::vector<double>& max_reproj_errors) :
    image_processing_(),
    conic_finder_(num_streams),
    target_(num_streams,
            calibu::TargetGridDot(grid_spacing, grid_size, grid_seed)),
    grid_size_(grid_size),
    grid_spacing_(grid_spacing),
    grid_seed_(grid_seed),
    calib_cams_(num_streams, 0),
    frame_times_(num_streams, 0),
    current_frame_time_(),
    nstreams_(num_streams),
    width_(width),
    height_(height),
    calib_frame_(-1),
    tracking_good_(num_streams, false),
    t_cw_(num_streams),
    stats_(),
    num_frames_(0),
    calibrator_(),
  input_cameras_(input_cameras),
    max_reproj_errors_(max_reproj_errors),

#ifdef HAVE_PANGOLIN
    textures_(nstreams_),
    stacks_(),
    imu_strips_(),
    handler_(stacks_),
#endif  // HAVE_PANGOLIN
    options_(nullptr) {
  for (size_t i = 0; i < nstreams_; ++i) {
    image_processing_.emplace_back(width[i], height[i]);
    image_processing_[i].Params().black_on_white = true;
    image_processing_[i].Params().at_threshold = 0.9;
    image_processing_[i].Params().at_window_ratio = 30.0;

    conic_finder_[i].Params().conic_min_area = 4.0;
    conic_finder_[i].Params().conic_min_density = 0.6;
    conic_finder_[i].Params().conic_min_aspect = 0.2;
  }
  calibrator_.FixCameraIntrinsics(fix_intrinsics);

  for (size_t ii = 0; ii < num_streams; ++ii) {
    const int w_i = width[ii];
    const int h_i = height[ii];
    if (ii < input_cameras.size()) {
      calib_cams_[ii] = calibrator_.AddCamera(input_cameras[ii].camera,
                                              input_cameras[ii].T_ck);
    } else {
      // Generic starting set of parameters.
      calibu::CameraModelT<calibu::Fov> starting_cam(w_i, h_i);
      starting_cam.Params() << 300, 300, w_i / 2.0, h_i / 2.0, 0.2;

      calib_cams_[ii] = calibrator_.AddCamera(calibu::CameraModel(starting_cam),
                                              Sophus::SE3d());
    }
  }
  SetupGUI();
}

VicalibTask::~VicalibTask() { }

void VicalibTask::SetupGUI() {
#ifdef HAVE_PANGOLIN
  // Setup GUI
  const int panel_width = 150;
  pangolin::CreateWindowAndBind("Main", (NumStreams() + 1) *
                                width() / 2.0 + panel_width,
                                height() / 2.0);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

  // Make things look prettier...
  glEnable(GL_LINE_SMOOTH);
  glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST);
  glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_DEPTH_TEST);
  glLineWidth(1.7);

  // Pangolin 3D Render state
  // Create viewport for video with fixed aspect
  pangolin::CreatePanel("ui").SetBounds(1.0, 0.0, 0,
                                        pangolin::Attach::Pix(panel_width));

  pangolin::View& container = pangolin::Display("vicalib").SetBounds(
      1.0, 0.0, pangolin::Attach::Pix(panel_width), 1.0).SetLayout(
          pangolin::LayoutEqual);

  // Add view for each camera stream
  for (size_t c = 0; c < NumStreams(); ++c) {
    container.AddDisplay(
        pangolin::CreateDisplay().SetAspect(
            width(c) / static_cast<float>(height(c))));
  }

  // Add 3d view, attach input handler_
  pangolin::View& v3d = pangolin::Display("vicalib3d").SetAspect(
      static_cast<float>(width()) / height()).SetHandler(&handler_);
  container.AddDisplay(v3d);

  // 1,2,3,... keys hide and show viewports
  for (size_t ii = 0; ii < container.NumChildren(); ++ii) {
    pangolin::RegisterKeyPressCallback(
        '1' + ii, [&container, ii]() {
          container[ii].ToggleShow();
        });
  }

  for (size_t i = 0; i < nstreams_; ++i) {
    std::stringstream ss;
    ss << i;
    std::string istr = ss.str();
    CVarUtils::AttachCVar("proc.adaptive.threshold_" + istr,
                          &image_processing_[i].Params().at_threshold);
    CVarUtils::AttachCVar("proc.adaptive.window_ratio_" + istr,
                          &image_processing_[i].Params().at_window_ratio);
    CVarUtils::AttachCVar("proc.black_on_white_" + istr,
                          &image_processing_[i].Params().black_on_white);
    textures_[i].Reinitialise(width(i), height(i), GL_LUMINANCE8);
  }

  options_.reset(new VicalibGuiOptions);
  stacks_.SetProjectionMatrix(
      pangolin::ProjectionMatrixRDF_TopLeft(640, 480, 420, 420, 320, 240, 0.01,
                                            1E6));
  stacks_.SetModelViewMatrix(
      pangolin::ModelViewLookAtRDF(0, 0, -0.5, 0, 0, 0, 0, -1, 0));

#endif  // HAVE_PANGOLIN
}

void VicalibTask::Start(const bool has_initial_guess) {
  calibrator_.SetOptimizationFlags(has_initial_guess, has_initial_guess,
                                   !has_initial_guess);
  calibrator_.SetFunctionTolerance(FLAGS_function_tolerance);
  calibrator_.Start();
}

bool VicalibTask::IsRunning() {
  return calibrator_.IsRunning();
}

int VicalibTask::AddFrame(double frame_time) {
  return calibrator_.AddFrame(Sophus::SE3d(Sophus::SO3d(),
                                           Eigen::Vector3d(0, 0, 1000)),
                              frame_time);
}

void VicalibTask::AddImageMeasurements(const std::vector<bool>& valid_frames) {
  size_t n = images_->Size();

  std::vector<Eigen::Vector2d,
              Eigen::aligned_allocator<Eigen::Vector2d> > ellipses[n];
  for (size_t ii = 0; ii < n; ++ii) {
    if (!valid_frames[ii]) {
      tracking_good_[ii] = false;
      continue;
    }

    pb::Image img = images_->at(ii);
    image_processing_[ii].Process(img.data(),
                                  img.Width(),
                                  img.Height(),
                                  img.Width());
    conic_finder_[ii].Find(image_processing_[ii]);

    const std::vector<calibu::Conic,
                      Eigen::aligned_allocator<calibu::Conic> >& conics =
        conic_finder_[ii].Conics();
    std::vector<int> ellipse_target__map;

    tracking_good_[ii] = target_[ii].FindTarget(image_processing_[ii],
                                                conics,
                                                ellipse_target__map);
    if (!tracking_good_[ii]) {
      continue;
    }

    // Generate map and point structures
    for (size_t i = 0; i < conics.size(); ++i) {
      ellipses[ii].push_back(conics[i].center);
    }

    // find camera pose given intrinsics
    PosePnPRansac(calibrator_.GetCamera(ii).camera, ellipses[ii],
                  target_[ii].Circles3D(), ellipse_target__map, 0, 0,
                  &t_cw_[ii]);
  }

  bool any_good = false;
  for (bool good : tracking_good_) {
    if (good) {
      any_good = true;
      break;
    }
  }
  if (!any_good) {
    LOG(WARNING) << "No well tracked frames found. Not adding frame.";
    return;
  }
  calib_frame_ = AddFrame(current_frame_time_);

  for (size_t ii = 0; ii < n; ++ii) {
    if (tracking_good_[ii]) {
      if (calib_frame_ >= 0) {
        if (ii == 0 || !tracking_good_[0]) {
          // Initialize pose of frame for least squares optimisation
          // this needs to be T_wh which is why we invert
          calibrator_.GetFrame(calib_frame_)->t_wp = t_cw_[ii].inverse()
              * calibrator_.GetCamera(ii).T_ck;
        }

        for (size_t p = 0; p < ellipses[ii].size(); ++p) {
          const Eigen::Vector2d pc = ellipses[ii][p];
          const Eigen::Vector2i pg = target_[ii].Map()[p].pg;

          if (0 <= pg(0) && pg(0) < grid_size_(0) && 0 <= pg(1)
              && pg(1) < grid_size_(1)) {
            const Eigen::Vector3d pg3d = grid_spacing_
                * Eigen::Vector3d(pg(0), pg(1), 0);
            // TODO(nimski): Add these correspondences in bulk to
            // avoid hitting mutex each time.
            calibrator_.AddObservation(calib_frame_, calib_cams_[ii], pg3d, pc,
                                       current_frame_time_);
          }
        }
      }
    }
  }
}

#ifdef HAVE_PANGOLIN

void VicalibTask::Draw3d() {
  pangolin::View& v3d = pangolin::Display("vicalib3d");
  if (!v3d.IsShown()) return;

  v3d.ActivateScissorAndClear(stacks_);
  calibu::glDrawTarget(target_[0], Eigen::Vector2d(0, 0), 1.0, 0.8, 1.0);

  if (options_->disp_barcode) {
    const std::vector<Eigen::Vector3d,
                      Eigen::aligned_allocator<Eigen::Vector3d> >& code_points =
        target_[0].Code3D();
    for (const Eigen::Vector3d& xwp : code_points) {
      glColor3f(1.0, 0.0, 1.0);
      pangolin::glDrawCircle(xwp.head<2>(), target_[0].CircleRadius());
    }
  }

  for (size_t c = 0; c < calibrator_.NumCameras(); ++c) {
    const Eigen::Matrix3d k_inv = calibrator_.GetCamera(c).camera.Kinv();

    const CameraAndPose cap = calibrator_.GetCamera(c);
    const Sophus::SE3d t_ck = cap.T_ck;

    // Draw keyframes
    pangolin::glColorBin(c, 2, 0.2);
    for (size_t k = 0; k < calibrator_.NumFrames(); ++k) {
      // Draw the camera frame if we had measurements from it
      if (calibrator_.GetFrame(k)->has_measurements_from_cam[c]) {
        pangolin::glDrawAxis(
            (calibrator_.GetFrame(k)->t_wp * t_ck.inverse()).matrix(), 0.01);
      }

      // Draw the IMU frame
      pangolin::glDrawAxis((calibrator_.GetFrame(k)->t_wp).matrix(), 0.02);

      glColor4f(1, 1, 1, 1);
      // also draw the imu integration for this pose
      if (k < calibrator_.NumFrames() - 1) {
        if (k >= imu_strips_.size()) {
          // also add a strip for the imu integration of this
          // frame to be displayed
          imu_strips_.push_back(std::unique_ptr<GLLineStrip>(new GLLineStrip));
        }
        // now set the points on the strip based on the imu integration
        const std::vector<visual_inertial_calibration::ImuPoseT<double>,
                          Eigen::aligned_allocator<
                            visual_inertial_calibration::ImuPoseT<double> > >
            poses = calibrator_.GetIntegrationPoses(k);
        Eigen::Vector3dAlignedVec v3d_poses;
        v3d_poses.reserve(poses.size());
        for (const auto& p : poses) {
          v3d_poses.push_back((p.t_wp).translation());
        }
        imu_strips_[k]->SetPointsFromTrajectory(v3d_poses);
        imu_strips_[k]->Draw();
      }
    }

    // Draw current camera
    if (tracking_good_[c]) {
      pangolin::glColorBin(c, 2, 0.5);
      pangolin::glDrawFrustrum(k_inv, width(c), height(c),
                               t_cw_[c].inverse().matrix(),
                               0.05);
    }
  }
}

void VicalibTask::Draw2d() {
  for (size_t c = 0; c < calibrator_.NumCameras(); ++c) {
    pangolin::View& container = pangolin::Display("vicalib");
    if (container[c].IsShown()) {
      container[c].ActivateScissorAndClear();
      glColor3f(1, 1, 1);

      // Display camera image
      if (!options_->disp_thresh) {
        textures_[c].Upload(image_processing_[c].Img(),
                            GL_LUMINANCE, GL_UNSIGNED_BYTE);
        textures_[c].RenderToViewportFlipY();
      } else {
        textures_[c].Upload(image_processing_[c].ImgThresh(), GL_LUMINANCE,
                            GL_UNSIGNED_BYTE);
        textures_[c].RenderToViewportFlipY();
      }

      // Setup orthographic pixel drawing
      glMatrixMode(GL_PROJECTION);
      glLoadIdentity();
      glOrtho(-0.5, width(c) - 0.5, height(c) - 0.5, -0.5, 0, 1.0);
      glMatrixMode(GL_MODELVIEW);

      const std::vector<calibu::Conic,
                        Eigen::aligned_allocator<calibu::Conic> >& conics =
          conic_finder_[c].Conics();
      if (options_->disp_lines) {
        for (std::list<calibu::LineGroup>::const_iterator it =
                 target_[c].LineGroups().begin();
             it != target_[c].LineGroups().end(); ++it) {
          glColor3f(0.5, 0.5, 0.5);
          glBegin(GL_LINE_STRIP);
          for (std::list<size_t>::const_iterator el = it->ops.begin();
               el != it->ops.end(); ++el) {
            const Eigen::Vector2d p = conics[*el].center;
            glVertex2d(p(0), p(1));
          }
          glEnd();
        }
      }

      if (tracking_good_[c] && options_->disp_barcode) {
        static const int kImageBoundaryRegion = 10;
        const std::vector<Eigen::Vector3d,
            Eigen::aligned_allocator<Eigen::Vector3d> >& code_points =
                target_[c].Code3D();
        const CameraAndPose cap = calibrator_.GetCamera(c);
        const unsigned char* im = image_processing_[c].ImgThresh();
        unsigned char id = 0;
        bool found = true;
        for (size_t c = 0; c < code_points.size(); c++) {
          const Eigen::Vector3d& xwp = code_points[c];
          Eigen::Vector2d pt;
          pt = cap.camera.Project(t_cw_[c] * xwp);
          if (pt[0] < kImageBoundaryRegion ||
              pt[0] >= images_->at(c).Width() - kImageBoundaryRegion ||
              pt[1] < kImageBoundaryRegion ||
              pt[1] >= images_->at(c).Height() - kImageBoundaryRegion) {
            found = false;
            continue;
          }

          int idx = image_processing_[c].Width() * round(pt[1]) + round(pt[0]);
          if (im[idx] == 0) {
            glColor3f(0.0, 1.0, 0.0);
            id |= 1 << c;
          } else {
            glColor3f(1.0, 0.0, 0.0);
          }
          pangolin::glDrawRect(pt[0] - 5, pt[1] - 5, pt[0] + 5, pt[1] + 5);
        }
        if (found) {
          LOG(INFO) << "ID: " << id;
        }
      }

      if (options_->disp_cross) {
        for (size_t i = 0; i < conics.size(); ++i) {
          const Eigen::Vector2d pc = conics[i].center;
          pangolin::glColorBin(target_[c].Map()[i].value, 2);
          pangolin::glDrawCross(pc, conics[i].bbox.Width() * 0.75);
        }
      }

      if (options_->disp_bbox) {
        for (size_t i = 0; i < conics.size(); ++i) {
          const Eigen::Vector2i grid_point =
              tracking_good_[c] ?
              target_[c].Map()[i].pg : Eigen::Vector2i(0, 0);

          if (0 <= grid_point[0] && grid_point[0] < grid_size_[0] &&
              0 <= grid_point[1] && grid_point[1] < grid_size_[1]) {
            pangolin::glColorBin(grid_point[1] * grid_size_[0] + grid_point[0],
                                 grid_size_[0] * grid_size_[1]);
            calibu::glDrawRectPerimeter(conics[i].bbox);
          }
        }
      }
    }
  }
}
#endif  // HAVE_PANGOLIN

void VicalibTask::Draw() {
#ifdef HAVE_PANGOLIN
  options_->disp_mse = calibrator_.MeanSquaredError();
  options_->disp_frame = num_frames_;

  Draw2d();
  Draw3d();
#endif  // HAVE_PANGOLIN
}

void VicalibTask::Finish(const std::string& output_filename) {
  calibrator_.Stop();
  calibrator_.PrintResults();
}

std::vector<bool> VicalibTask::AddSuperFrame(
    const std::shared_ptr<pb::ImageArray>& images) {
  images_ = images;

  int num_new_frames = 0;
  std::vector<bool> valid_frames;
  valid_frames.resize(images_->Size());

  LOG(INFO) << "Frame timestamps: ";
  for (int ii = 0; ii < images_->Size(); ++ii) {
    LOG(INFO) << std::fixed << images_->at(ii).Timestamp();
    if (images_->at(ii).Timestamp() != frame_times_[ii]) {
      num_new_frames++;
      frame_times_[ii] = images_->at(ii).Timestamp();
      valid_frames[ii] = true;
    } else {
      valid_frames[ii] = false;
    }
  }

  // This is true if we have at least a single new frame
  bool is_new_frame = num_new_frames > 0;

  if (is_new_frame) {
    current_frame_time_ = frame_times_[0];
    LOG(INFO) << "Adding super frame at time " << current_frame_time_;

    AddImageMeasurements(valid_frames);
  } else if (!is_new_frame) {
    LOG(INFO) << "Frame duplicated";
  }

  num_frames_++;
  return valid_frames;
}

void VicalibTask::AddIMU(const pb::ImuMsg& imu) {
  if (!imu.has_accel()) {
    LOG(ERROR) << "ImuMsg missing accelerometer readings";
  } else if (!imu.has_gyro()) {
    LOG(ERROR) << "ImuMsg missing gyro readings";
  } else {
    Eigen::VectorXd gyro, accel;
    ReadVector(imu.accel(), &accel);
    ReadVector(imu.gyro(), &gyro);
    if (calibrator_.AddImuMeasurements(gyro, accel, imu.device_time())) {
      LOG(INFO) << "TIMESTAMPINFO: IMU Packet "
                << std::fixed << imu.device_time();
    }
  }
}

bool CalibrationsDiffer(const CameraAndPose& last,
                        const CameraAndPose& current) {
  if (last.camera.Type() != current.camera.Type()) {
    LOG(ERROR) << "Camera calibrations are different types: "
               << last.camera.Type() << " vs. "
               << current.camera.Type();
    return true;
  }

  Eigen::VectorXd lastParams = last.camera.GenericParams();
  Eigen::VectorXd currentParams = current.camera.GenericParams();

  LOG(INFO) << "Comparing old camera calibration: " << lastParams
            << " to new calibration: " << currentParams;

  if (std::abs(lastParams[0] - currentParams[0]) > FLAGS_max_fx_diff) {
    LOG(ERROR) << "fx differs too much ("
               << lastParams[0] - currentParams[0] << ")";
    return true;
  } else if (std::abs(lastParams[1] - currentParams[1]) > FLAGS_max_fy_diff) {
    LOG(ERROR) << "fy differs too much ("
               << lastParams[1] - currentParams[1] << ")";
    return true;
  } else if (std::abs(lastParams[2] - currentParams[2]) > FLAGS_max_cx_diff) {
    LOG(ERROR) << "cx differs too much ("
               << lastParams[2] - currentParams[2] << ")";
    return true;
  } else if (std::abs(lastParams[3] - currentParams[3]) > FLAGS_max_cy_diff) {
    LOG(ERROR) << "cy differs too much ("
               << lastParams[3] - currentParams[3] << ")";
    return true;
  }

  if (current.camera.Type() == "calibu_fu_fv_u0_v0" &&
      std::abs(lastParams[4] - currentParams[4]) > FLAGS_max_fov_w_diff) {
    LOG(ERROR) << "fov distortion differs too much ("
               << lastParams[4] - currentParams[4] << ")";
    return true;
  } else if (current.camera.Type() == "calibu_fu_fv_u0_v0_k1_k2_k3") {
    double d1 = std::abs(lastParams[4] - currentParams[4]);
    double d2 = std::abs(lastParams[5] - currentParams[5]);
    double d3 = std::abs(lastParams[6] - currentParams[6]);

    if (d1 > FLAGS_max_poly3_diff_k1 ||
        d2 > FLAGS_max_poly3_diff_k2 ||
        d3 > FLAGS_max_poly3_diff_k3) {
      LOG(ERROR) << "poly3 distortion differs too much ("
                 << d1 << ", " << d2 << ", " << d3 << ")";
      return true;
    }
  }

  return false;
}

bool VicalibTask::IsSuccessful() const {
  std::vector<double> errors = calibrator_.GetCameraProjRMSE();
  for (size_t i = 0; i < nstreams_; ++i) {
    if (errors[i] > max_reproj_errors_[i]) {
      LOG(WARNING) << "Reprojection error of " << errors[i]
                   << " was greater than maximum of "
                   << max_reproj_errors_[i]
                   << " for camera " << i;
      return false;
    }
  }

  // Check that camera parameters are within thresholds of previous
  // run if we initialized our optimization with it
  if (FLAGS_has_initial_guess) {
    for (size_t i = 0; i < input_cameras_.size(); ++i) {
      if (CalibrationsDiffer(input_cameras_[i], calibrator_.GetCamera(i))) {
        return false;
      }
    }
  }
  return true;
}
}  // namespace visual_inertial_calibration