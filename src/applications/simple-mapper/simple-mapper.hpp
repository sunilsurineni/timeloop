/* Copyright (c) 2019, NVIDIA CORPORATION. All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <fstream>
#include <iomanip>

#include <boost/serialization/vector.hpp>
#include <boost/serialization/array.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/bitset.hpp>

#include "util/accelergy_interface.hpp"
#include "mapspaces/mapspace-factory.hpp"
#include "compound-config/compound-config.hpp"

//--------------------------------------------//
//                Application                 //
//--------------------------------------------//

class Application
{
 protected:

  problem::Workload workload_;
  model::Engine::Specs arch_specs_;
  mapspace::MapSpace* mapspace_;

  std::string out_prefix_ = "timeloop-mapper";

 public:

  Application(config::CompoundConfig* config)
  {
    auto rootNode = config->getRoot();

    // Problem configuration.
    auto problem = rootNode.lookup("problem");
    problem::ParseWorkload(problem, workload_);

    // Architecture configuration.
    config::CompoundConfigNode arch;
    arch = rootNode.lookup("architecture");
    arch_specs_ = model::Engine::ParseSpecs(arch);

#ifdef USE_ACCELERGY
    if (arch.exists("subtree") || arch.exists("local"))
    {
      accelergy::invokeAccelergy(config->inFiles, out_prefix_);
      std::string ertPath = out_prefix_ + ".ERT.yaml";
      auto ertConfig = new config::CompoundConfig(ertPath.c_str());
      auto ert = ertConfig->getRoot().lookup("ERT");
      arch_specs_.topology.ParseAccelergyERT(ert);
    }
#endif

  

       // MapSpace configuration.
    if (rootNode.exists("mapspace"))
    {
      auto mapspace = rootNode.lookup("mapspace");
      mapspace_ = mapspace::ParseAndConstruct(mapspace, arch_specs_, workload_);
    }
    else if (rootNode.exists("mapspace_constraints"))
    {
      auto mapspace = rootNode.lookup("mapspace_constraints");
      mapspace_ = mapspace::ParseAndConstruct(mapspace, arch_specs_, workload_);      
    }
    else
    {
      std::cerr << "ERROR: found neither \"mapspace\" nor \"mapspace_constraints\" "
                << "directive. To run the mapper without any constraints set "
                << "mapspace_constraints as an empty list []." << std::endl;
      exit(1);
    }
  }

  ~Application()
  {
    if (mapspace_)
    {
      delete mapspace_;
    }
  }

  // ---------------
  // Run the mapper.
  // ---------------
  void Run()
  {
    // Output file names.
    const std::string stats_file_name = out_prefix_ + ".stats.txt";
    const std::string map_txt_file_name = out_prefix_ + ".map.txt";
    
    Mapping best_mapping;
    model::Engine best_engine;
    model::Engine engine;

    // =================
    // Main mapper loop.
    // =================
    for (uint128_t i = 0; i < mapspace_->Size(mapspace::Dimension::IndexFactorization); i++)
      for (uint128_t j = 0; j < mapspace_->Size(mapspace::Dimension::LoopPermutation); j++)
        for (uint128_t k = 0; k < mapspace_->Size(mapspace::Dimension::Spatial); k++)
          for (uint128_t l = 0; l < mapspace_->Size(mapspace::Dimension::DatatypeBypass); l++)
          {
            // Prepare a new mapping ID.
            mapspace::ID mapping_id = mapspace::ID(mapspace_->AllSizes());

            mapping_id.Set(int(mapspace::Dimension::IndexFactorization), i);
            mapping_id.Set(int(mapspace::Dimension::LoopPermutation), j);
            mapping_id.Set(int(mapspace::Dimension::Spatial), k);
            mapping_id.Set(int(mapspace::Dimension::DatatypeBypass), l);

            // We should probably hoist the i, j and k Set() calls above to the
            // loop levels where they are actually changing. Alternatively, since
            // we are simply walking through the space linearly, we can simply
            // use mapping_id.Increment(), it will walk across all dimensions.

            // Construct a mapping from the mapping ID. This step can fail
            // because the space of *legal* mappings isn't dense (unfortunately),
            // so a mapping ID may point to an illegal mapping.
            Mapping mapping;
            bool success = mapspace_->ConstructMapping(mapping_id, &mapping);
            if (!success)
            {
              continue;
            }

            // Configure the model and evaluate the mapping.
            //engine.Spec(arch_specs_);
            auto status_per_level = engine.Evaluate(mapping, workload_);
            success = std::accumulate(status_per_level.begin(), status_per_level.end(), true,
                                      [](bool cur, const model::EvalStatus& status)
                                      { return cur && status.success; });
            if (!success)
            {
              continue;
            }

            // Is the new mapping "better" than the previous best mapping?
            // We are probing the energy consumption of the last-evaluated mapping, but
            // we can probe any stat that the model (engine) generates.
            if (!best_engine.IsSpecced() || engine.Energy() < best_engine.Energy())
            {
              best_mapping = mapping;
              best_engine = engine;          
            }
          }
    
    if (best_engine.IsEvaluated())
    {
      std::ofstream map_txt_file(map_txt_file_name);
      best_mapping.PrettyPrint(map_txt_file, arch_specs_.topology.StorageLevelNames(),
                               best_engine.GetTopology().TileSizes());
      map_txt_file.close();

      std::ofstream stats_file(stats_file_name);
      stats_file << best_engine << std::endl;
      stats_file.close();

      std::cout << std::endl;
      std::cout << "Summary stats for best mapping found by mapper:" << std::endl; 
      std::cout << "  Utilization = " << std::setw(4) << std::fixed << std::setprecision(2)
                << best_engine.Utilization() << " | pJ/MACC = " << std::setw(8)
                << std::fixed << std::setprecision(3) << best_engine.Energy() /
        best_engine.GetTopology().MACCs() << std::endl;
    }
    else
    {
      std::cout << "MESSAGE: no valid mappings found within search criteria." << std::endl;
    }
  }
};
