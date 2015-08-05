// -*- mode: c++; indent-tabs-mode: nil; tab-width: 2 -*-
#pragma once
#include <string>
#include "moses/Parameter.h"
namespace Moses
{

  struct 
  ReorderingOptions 
  {
    int max_distortion;
    bool monotone_at_punct;
    bool use_early_distortion_cost;
    bool init(Parameter const& param);
    ReorderingOptions(Parameter const& param);
    ReorderingOptions() {}
  };

}

