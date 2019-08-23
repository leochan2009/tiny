const char *kUsageMessage = "Detect features in the images and group them " \
    "into tracks";
#include <algorithm>
#include <cstdint>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/features2d/features2d.hpp>
#include <glog/logging.h>

#include "furry/common/init.h"
#include "furry/common/str.h"
#include "furry/common/cv.h"
#include "furry/common/sequence.h"
#include "furry/gui/window_QT.h"

#include "model.h"

using namespace cv;
using namespace furry;
using namespace furry::tiny;
using namespace std;

DEFINE_string(in_model, "", "");
DEFINE_string(in_image_dir, "", "");
DEFINE_string(in_image_pattern, "", "");
DEFINE_string(out_model, "", "");
DEFINE_bool(show, false, "");
DEFINE_bool(record, false, "");
DEFINE_string(out_video, "", "");
DEFINE_int32(num_images, -1, "");
DEFINE_int32(ref_image, 0, "");
DEFINE_int32(max_corners, 10000, "");
DEFINE_double(corner_quality_level, 0.01, "");
DEFINE_double(corner_min_distance, 5, "");
DEFINE_int32(corner_block_size, 3, "");
DEFINE_int32(lk_window_size, 11, "");
DEFINE_int32(lk_max_level, 3, "");
DEFINE_double(lk_error_threshold, 6, "");

void FindGoodTracks(const vector<vector<uint8_t>> &status,
                    const vector<vector<float>> &errors,
                    vector<uint8_t> *complete) {
  int num_tracks = status[0].size();
  complete->resize(num_tracks, 1);
  for (size_t i = 0; i < status.size(); ++i) {
    for (int j = 0; j < num_tracks; ++j) {
      if (errors[i][j] > FLAGS_lk_error_threshold) {
        (*complete)[j] = 0;
      }
    }
  }
}

int main(int argc, char *argv[]) {
  furry::Init(&argc, &argv, kUsageMessage);

  Model model;
  if (!FLAGS_in_model.empty()) {
    model.ReadFile(FLAGS_in_model);
  } else if (!FLAGS_in_image_dir.empty()) {
    model.ReadImageDir(FLAGS_in_image_dir, FLAGS_in_image_pattern);
  } else {
    LOG(FATAL) << "No model input";
  }

  int num_images = (FLAGS_num_images < 0) ? model.NumCameras() :
      min(FLAGS_num_images, model.NumCameras());
  if (num_images < model.NumCameras()) {
    model.RemoveCameras(num_images, model.NumCameras());
  }
  vector<Mat> color_images(num_images);
  vector<Mat> images(num_images);
  ProgressBar reading_images("Reading Images", num_images);
  // for (int i = 0; i < num_images; ++i) {
  ParFor(0, num_images, [&](int i) {
      color_images[i] = model.GetCamera(i)->ReadImage();
      cvtColor(color_images[i], images[i], CV_BGR2GRAY);
      reading_images.FinishOne();
    });
  reading_images.Done();

  model.RemoveTracks();

  tbb::mutex model_mutex;

  // for (ref_image = 0; ref_image < num_images; ++ref_image) {
  ProgressBar tracking_corners("Tracking Corners", num_images / 10);
  ParFor(0, num_images, [&](int ref_image) {
      if (ref_image % 10 != 0) return;
      vector<vector<Point2f>> corners(num_images);
      vector<vector<uint8_t>> status(num_images);
      vector<vector<float>> errors(num_images);
      goodFeaturesToTrack(images[ref_image], corners[ref_image],
                          FLAGS_max_corners,
                          FLAGS_corner_quality_level,
                          FLAGS_corner_min_distance,
                          cv::noArray(),
                          FLAGS_corner_block_size);
      int num_corners = corners[ref_image].size();
      // LOG(INFO) << "# corners: " << num_corners;
      cornerSubPix(images[ref_image], corners[ref_image],
                   Size(FLAGS_lk_window_size / 2, FLAGS_lk_window_size / 2),
                   Size(-1, -1),
                   TermCriteria(TermCriteria::COUNT+TermCriteria::EPS, 30, 0.01));
      status[ref_image].resize(num_corners, 1);
      errors[ref_image].resize(num_corners, 0);

      for (int i = 0 ; i < num_images; ++i) {
        // ParFor(0, num_images, [&](int i) {
        // if (i == ref_image) return;
        if (i == ref_image) continue;
        calcOpticalFlowPyrLK(images[ref_image], images[i], corners[ref_image],
                             corners[i], status[i], errors[i],
                             Size(FLAGS_lk_window_size, FLAGS_lk_window_size),
                             FLAGS_lk_max_level);
      }

      vector<uint8_t> complete;
      // FindGoodTracks(status, errors, &complete);
      // for (int i = 0; i < num_images; ++i) {
      //   corners[i] = Filter(corners[i], complete);
      // }
      num_corners = corners[0].size();

      vector<Point2f> detections;
      for (int i = 0; i < num_corners; ++i) {
        auto track = new Track;
        track->SetPoint(0, 0, 0);
        track->SetColor(0, 0, 0);
        detections.resize(0);
        for (int j = 0; j < num_images; ++j) {
          if (status[j][i] != 0 && errors[j][i] < FLAGS_lk_error_threshold) {
            track->AddCameraId(model.GetCamera(j)->GetId());
            detections.push_back(corners[j][i]);
          }
        }
        {
          tbb::mutex::scoped_lock lock(model_mutex);
          model.AddTrack(track, detections);
        }
      }
      tracking_corners.FinishOne();
    });
  tracking_corners.Done();

  // model.ColorTracks();

  if (!FLAGS_out_model.empty()) {
    model.WriteFile(FLAGS_out_model);
  }

  if (FLAGS_record) {
    CHECK_NE(FLAGS_out_video, "");
    VLOG(1) << "Init video writer\n";
    VideoWriter video_writer(FLAGS_out_video,
                             // CV_FOURCC('M', 'P', 'G', '4'),
                             CV_FOURCC('P','I','M','1'),
                             30,
                             model.GetCamera(0)->ReadImage().size());
    ProgressBar recording("Recording", num_images);
    for (int i = 0; i < num_images; ++i) {
      video_writer << model.GetCamera(i)->ShowDetections();
      recording.FinishOne();
    }
    recording.Done();
  }

  if (FLAGS_show) {
    QApplication app(argc, argv);
    CvWindow window("Detect Tracks");
    int index = 0;
    window.UpdateImage(model.GetCamera(index)->ShowDetections());
    window.SetKeyCallback(
        [&](char k) {
          switch (k) {
            case 'j':
              ++index;
              break;
            case 'k':
              --index;
              break;
            default:
              break;
          }
          index = (index + num_images) % num_images;
          window.UpdateImage(model.GetCamera(index)->ShowDetections());
          LOG(INFO) << "Drawing " << index << "th image";
        });
    app.exec();
  }

  return 0;
}
