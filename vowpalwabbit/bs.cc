/*
Copyright (c) by respective owners including Yahoo!, Microsoft, and
individual contributors. All rights reserved.  Released under a BSD (revised)
license as described in the file LICENSE.
 */
#include <cfloat>
#include <cmath>
#include <cerrno>
#include <sstream>
#include <numeric>
#include <vector>
#include <memory>

#include "reductions.h"
#include "rand48.h"
#include "vw.h"
#include "bs.h"
#include "vw_exception.h"

using namespace LEARNER;
using namespace VW::config;

struct bs
{
  uint32_t B;  // number of bootstrap rounds
  size_t bs_type;
  float lb;
  float ub;
  std::vector<double>* pred_vec;
  vw* all;  // for raw prediction and loss
  std::shared_ptr<rand_state> _random_state;

  ~bs()
  {
    delete pred_vec;
  }
};

void bs_predict_mean(vw& all, example& ec, std::vector<double>& pred_vec)
{
  ec.pred.scalar = (float)accumulate(pred_vec.cbegin(), pred_vec.cend(), 0.0) / pred_vec.size();
  if (ec.weight > 0 && ec.l.simple.label != FLT_MAX)
    ec.loss = all.loss->getLoss(all.sd, ec.pred.scalar, ec.l.simple.label) * ec.weight;
}

void bs_predict_vote(example& ec, std::vector<double>& pred_vec)
{
  // majority vote in linear time
  unsigned int counter = 0;
  int current_label = 1, init_label = 1;
  // float sum_labels = 0; // uncomment for: "avg on votes" and getLoss()
  bool majority_found = false;
  bool multivote_detected = false;  // distinct(votes)>2: used to skip part of the algorithm
  auto pred_vec_sz = pred_vec.size();
  int* pred_vec_int = new int[pred_vec_sz];

  for (unsigned int i = 0; i < pred_vec_sz; i++)
  {
    pred_vec_int[i] = (int)floor(
        pred_vec[i] + 0.5);  // could be added: link(), min_label/max_label, cutoff between true/false for binary

    if (!multivote_detected)  // distinct(votes)>2 detection bloc
    {
      if (i == 0)
      {
        init_label = pred_vec_int[i];
        current_label = pred_vec_int[i];
      }
      else if (init_label != current_label && pred_vec_int[i] != current_label && pred_vec_int[i] != init_label)
        multivote_detected = true;  // more than 2 distinct votes detected
    }

    if (counter == 0)
    {
      counter = 1;
      current_label = pred_vec_int[i];
    }
    else
    {
      if (pred_vec_int[i] == current_label)
        counter++;
      else
      {
        counter--;
      }
    }
  }

  if (counter > 0 && multivote_detected)  // remove this condition for: "avg on votes" and getLoss()
  {
    counter = 0;
    for (unsigned int i = 0; i < pred_vec.size(); i++)
      if (pred_vec_int[i] == current_label)
      {
        counter++;
        // sum_labels += pred_vec[i]; // uncomment for: "avg on votes" and getLoss()
      }
    if (counter * 2 > pred_vec.size())
      majority_found = true;
  }

  if (multivote_detected && !majority_found)  // then find most frequent element - if tie: smallest tie label
  {
    std::sort(pred_vec_int, pred_vec_int + pred_vec.size());
    int tmp_label = pred_vec_int[0];
    counter = 1;
    for (unsigned int i = 1, temp_count = 1; i < pred_vec.size(); i++)
    {
      if (tmp_label == pred_vec_int[i])
        temp_count++;
      else
      {
        if (temp_count > counter)
        {
          current_label = tmp_label;
          counter = temp_count;
        }
        tmp_label = pred_vec_int[i];
        temp_count = 1;
      }
    }
    /* uncomment for: "avg on votes" and getLoss()
    sum_labels = 0;
    for(unsigned int i=0; i<pred_vec.size(); i++)
      if(pred_vec_int[i] == current_label)
        sum_labels += pred_vec[i]; */
  }
  // TODO: unique_ptr would also handle exception case
  delete[] pred_vec_int;

  // ld.prediction = sum_labels/(float)counter; //replace line below for: "avg on votes" and getLoss()
  ec.pred.scalar = (float)current_label;

  // ec.loss = all.loss->getLoss(all.sd, ld.prediction, ld.label) * ec.weight; //replace line below for: "avg on votes"
  // and getLoss()
  ec.loss = ((ec.pred.scalar == ec.l.simple.label) ? 0.f : 1.f) * ec.weight;
}

void print_result(int f, float res, v_array<char> tag, float lb, float ub)
{
  if (f >= 0)
  {
    std::stringstream ss;
    ss << std::fixed << res;
    print_tag(ss, tag);
    ss << std::fixed << ' ' << lb << ' ' << ub << '\n';
    ssize_t len = ss.str().size();
    ssize_t t = io_buf::write_file_or_socket(f, ss.str().c_str(), (unsigned int)len);
    if (t != len)
      std::cerr << "write error: " << strerror(errno) << std::endl;
  }
}

void output_example(vw& all, bs& d, example& ec)
{
  label_data& ld = ec.l.simple;

  all.sd->update(ec.test_only, ld.label != FLT_MAX, ec.loss, ec.weight, ec.num_features);
  if (ld.label != FLT_MAX && !ec.test_only)
    all.sd->weighted_labels += ((double)ld.label) * ec.weight;

  if (!all.final_prediction_sink.empty())  // get confidence interval only when printing out predictions
  {
    d.lb = FLT_MAX;
    d.ub = -FLT_MAX;
    for (double v : *d.pred_vec)
    {
      if (v > d.ub)
        d.ub = (float)v;
      if (v < d.lb)
        d.lb = (float)v;
    }
  }

  for (int sink : all.final_prediction_sink) print_result(sink, ec.pred.scalar, ec.tag, d.lb, d.ub);

  print_update(all, ec);
}

template <bool is_learn>
void predict_or_learn(bs& d, single_learner& base, example& ec)
{
  vw& all = *d.all;
  bool shouldOutput = all.raw_prediction > 0;

  float weight_temp = ec.weight;

  std::stringstream outputStringStream;
  d.pred_vec->clear();

  for (size_t i = 1; i <= d.B; i++)
  {
    ec.weight = weight_temp * (float)BS::weight_gen(d._random_state);

    if (is_learn)
      base.learn(ec, i - 1);
    else
      base.predict(ec, i - 1);

    d.pred_vec->push_back(ec.pred.scalar);

    if (shouldOutput)
    {
      if (i > 1)
        outputStringStream << ' ';
      outputStringStream << i << ':' << ec.partial_prediction;
    }
  }

  ec.weight = weight_temp;

  switch (d.bs_type)
  {
    case BS_TYPE_MEAN:
      bs_predict_mean(all, ec, *d.pred_vec);
      break;
    case BS_TYPE_VOTE:
      bs_predict_vote(ec, *d.pred_vec);
      break;
    default:
      THROW("Unknown bs_type specified: " << d.bs_type);
  }

  if (shouldOutput)
    all.print_text(all.raw_prediction, outputStringStream.str(), ec.tag);
}

void finish_example(vw& all, bs& d, example& ec)
{
  output_example(all, d, ec);
  VW::finish_example(all, ec);
}

base_learner* bs_setup(options_i& options, vw& all)
{
  auto data = scoped_calloc_or_throw<bs>();
  std::string type_string("mean");
  option_group_definition new_options("Bootstrap");
  new_options.add(make_option("bootstrap", data->B).keep().help("k-way bootstrap by online importance resampling"))
      .add(make_option("bs_type", type_string).keep().help("prediction type {mean,vote}"));
  options.add_and_parse(new_options);

  if (!options.was_supplied("bootstrap"))
    return nullptr;

  data->ub = FLT_MAX;
  data->lb = -FLT_MAX;

  if (options.was_supplied("bs_type"))
  {
    if (type_string == "mean")
      data->bs_type = BS_TYPE_MEAN;
    else if (type_string == "vote")
      data->bs_type = BS_TYPE_VOTE;
    else
    {
      std::cerr << "warning: bs_type must be in {'mean','vote'}; resetting to mean." << std::endl;
      data->bs_type = BS_TYPE_MEAN;
    }
  }
  else  // by default use mean
    data->bs_type = BS_TYPE_MEAN;

  data->pred_vec = new std::vector<double>();
  data->pred_vec->reserve(data->B);
  data->all = &all;
  data->_random_state = all.get_random_state();

  learner<bs, example>& l = init_learner(
      data, as_singleline(setup_base(options, all)), predict_or_learn<true>, predict_or_learn<false>, data->B);
  l.set_finish_example(finish_example);

  return make_base(l);
}
