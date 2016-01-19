#include <algorithm>
#include <functional>
#include <utility>
#include <vector>

#include "caffe/layers/prior_box_layer.hpp"

namespace caffe {

template <typename Dtype>
void PriorBoxLayer<Dtype>::LayerSetUp(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const PriorBoxParameter& prior_box_param =
      this->layer_param_.prior_box_param();
  CHECK(prior_box_param.has_min_size()) << "must provide min_size.";
  min_size_ = prior_box_param.min_size();
  CHECK_GT(min_size_, 0) << "min_size must be positive.";
  CHECK(prior_box_param.has_max_size()) << "must provide max_size.";
  max_size_ = prior_box_param.max_size();
  CHECK_GT(max_size_, 0) << "max_size must be positive.";
  aspect_ratios_.clear();
  aspect_ratios_.push_back(1.);
  flip_ = prior_box_param.flip();
  for (int i = 0; i < prior_box_param.aspect_ratio_size(); ++i) {
    float ar = prior_box_param.aspect_ratio(i);
    bool already_exist = false;
    for (int j = 0; j < aspect_ratios_.size(); ++j) {
      if (fabs(ar - aspect_ratios_[j]) < 1e-6) {
        already_exist = true;
        break;
      }
    }
    if (!already_exist) {
      aspect_ratios_.push_back(ar);
      if (flip_) {
        aspect_ratios_.push_back(1./ar);
      }
    }
  }
  num_priors_ = aspect_ratios_.size() + 1;
  clip_ = prior_box_param.clip();

  prev_width_ = -1;
  prev_height_ = -1;
}

template <typename Dtype>
void PriorBoxLayer<Dtype>::Reshape(const vector<Blob<Dtype>*>& bottom,
      const vector<Blob<Dtype>*>& top) {
  const int layer_width = bottom[0]->width();
  const int layer_height = bottom[0]->height();
  vector<int> top_shape(3, 1);
  top_shape[0] = bottom[0]->num();
  // 2 channels. First channel stores the mean of each prior coordinate.
  // Second channel stores the variance (e.g. 0.1) of each prior coordinate.
  top_shape[1] = 2;
  top_shape[2] = layer_width * layer_height * num_priors_ * 4;
  top[0]->Reshape(top_shape);
}

template <typename Dtype>
void PriorBoxLayer<Dtype>::Forward_cpu(const vector<Blob<Dtype>*>& bottom,
    const vector<Blob<Dtype>*>& top) {
  const int num = bottom[0]->num();
  const int layer_width = bottom[0]->width();
  const int layer_height = bottom[0]->height();
  const int img_width = bottom[1]->width();
  const int img_height = bottom[1]->height();
  const float step_x = static_cast<float>(img_width) / layer_width;
  const float step_y = static_cast<float>(img_height) / layer_height;
  Dtype* top_data = top[0]->mutable_cpu_data();
  int dim = layer_height * layer_width * num_priors_ * 4;
  // Only recompute the priors if current layer is of different size from
  // previous batch.
  if (layer_width != prev_width_ || layer_width != prev_height_) {
    for (int i = 0; i < num; ++i) {
      int idx = 0;
      for (int h = 0; h < layer_height; ++h) {
        for (int w = 0; w < layer_width; ++w) {
          float center_x = (w + 0.5) * step_x;
          float center_y = (h + 0.5) * step_y;
          float box_width, box_height;
          // first prior: aspect_ratio = 1, size = min_size
          box_width = box_height = min_size_;
          // xmin
          top_data[idx++] = (center_x - box_width / 2.) / img_width;
          // ymin
          top_data[idx++] = (center_y - box_height / 2.) / img_height;
          // xmax
          top_data[idx++] = (center_x + box_width / 2.) / img_width;
          // ymax
          top_data[idx++] = (center_y + box_height / 2.) / img_height;

          // second prior: aspect_ratio = 1, size = sqrt(min_size * max_size)
          box_width = box_height =
              sqrt(static_cast<float>(min_size_ * max_size_));
          // xmin
          top_data[idx++] = (center_x - box_width / 2.) / img_width;
          // ymin
          top_data[idx++] = (center_y - box_height / 2.) / img_height;
          // xmax
          top_data[idx++] = (center_x + box_width / 2.) / img_width;
          // ymax
          top_data[idx++] = (center_y + box_height / 2.) / img_height;

          // rest of priors
          for (int r = 0; r < aspect_ratios_.size(); ++r) {
            float ar = aspect_ratios_[r];
            if (fabs(ar - 1.) < 1e-6) {
              continue;
            }
            box_width = min_size_ * sqrt(ar);
            box_height = min_size_ / sqrt(ar);
            // xmin
            top_data[idx++] = (center_x - box_width / 2.) / img_width;
            // ymin
            top_data[idx++] = (center_y - box_height / 2.) / img_height;
            // xmax
            top_data[idx++] = (center_x + box_width / 2.) / img_width;
            // ymax
            top_data[idx++] = (center_y + box_height / 2.) / img_height;
          }
        }
      }
      // clip the prior's coordidate such that it is within [0, 1]
      if (clip_) {
        for (int d = 0; d < dim; ++d) {
          top_data[d] = std::min<Dtype>(std::max<Dtype>(top_data[d], 0.), 1.);
        }
      }
      // set the variance.
      top_data += top[0]->offset(0, 1);
      caffe_set<Dtype>(dim, Dtype(0.1), top_data);
      // go to next example.
      top_data += top[0]->offset(0, 1);
    }
    // Update the prior data.
    prior_data_.ReshapeLike(*(top[0]));
    caffe_copy<Dtype>(top[0]->count(), top[0]->cpu_data(),
                      prior_data_.mutable_cpu_data());
    // Update the size info.
    prev_width_ = layer_width;
    prev_height_ = layer_height;
  } else {
    // Double check the history data indeed has same size as current batch.
    CHECK_EQ(prior_data_.count(),
             num * 2 * layer_width * layer_height * num_priors_ * 4);
    caffe_copy<Dtype>(prior_data_.count(), prior_data_.cpu_data(), top_data);
  }
}

INSTANTIATE_CLASS(PriorBoxLayer);
REGISTER_LAYER_CLASS(PriorBox);

}  // namespace caffe
