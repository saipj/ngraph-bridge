//*****************************************************************************
// Copyright 2017-2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include "ngraph/ngraph.hpp"
#include "ngraph/opsets/opset.hpp"

#include "logging/ngraph_log.h"
#include "ngraph_bridge/default_opset.h"
#include "ngraph_bridge/executable.h"
#include "ngraph_bridge/ie_tensor.h"
#include "ngraph_bridge/openvino/ie_manager.h"

using namespace std;
using namespace ngraph;

namespace tensorflow {
namespace ngraph_bridge {

Executable::Executable(shared_ptr<Function> func, string device)
    : m_device{device}, m_trivial_fn{nullptr}, m_function(func) {
  NGRAPH_VLOG(2) << "Checking for unsupported ops in IE backend";
  auto start_unsp = std::chrono::high_resolution_clock::now();
  const auto& opset = ngraph::get_opset3();
  for (const auto& node : func->get_ops()) {
    if (!opset.contains_op_type(node.get())) {
      NGRAPH_VLOG(0) << "UNSUPPORTED OP DETECTED: "
                     << node->get_type_info().name;
      THROW_IE_EXCEPTION << "Detected op not belonging to opset3!";
    }
  }
  auto finish_unsp = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_unsp = finish_unsp - start_unsp;
  std::cout << "Unsupported Ops Check Time: " << elapsed_unsp.count() << " s\n";

  auto start_trv = std::chrono::high_resolution_clock::now();
  // A trivial function is one of
  //  1. constant function (Const -> Result)
  //  2. identity function (Parameter -> Result)
  //  3. zero function (* -> Zero)
  NGRAPH_VLOG(2) << "Checking for trivial functions in IE backend";
  bool trivial_fn = true;
  for (auto result : func->get_results()) {
    auto parent = result->input_value(0).get_node_shared_ptr();
    auto& shape = result->get_shape();
    trivial_fn &= ngraph::is_type<opset::Parameter>(parent) ||
                  ngraph::is_type<opset::Constant>(parent) ||
                  count(shape.begin(), shape.end(), 0);
  }
  auto finish_trv = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_trv = finish_trv - start_trv;
  std::cout << "Trivial Func Check Time: " << elapsed_trv.count() << " s\n";

  if (trivial_fn) {
    NGRAPH_VLOG(2) << "Function is trivial and can be short-circuited";
    m_trivial_fn = func;
    return;
  }

  NGRAPH_VLOG(2) << "Checking for function parameters in IE backend";
  if (func->get_parameters().size() == 0) {
    auto start_param = std::chrono::high_resolution_clock::now();
    NGRAPH_VLOG(1) << "No parameters found in nGraph function!";
    // Try to find a node that can be converted into a "static input"
    bool param_replaced = false;
    for (const auto& node : func->get_ordered_ops()) {
      // Only try to convert constant nodes at the edge to parameters
      // FIXME: IE cannot handle input parameters with i64/u6 precision
      // at the moment
      if (node->get_input_size() == 0 && ngraph::op::is_constant(node) &&
          !(node->get_element_type() == ngraph::element::i64 ||
            node->get_element_type() == ngraph::element::u64)) {
        auto constant = ngraph::as_type_ptr<opset::Constant>(node);
        auto element_type = constant->get_element_type();
        auto shape = constant->get_shape();
        auto param = std::make_shared<opset::Parameter>(element_type, shape);
        param->set_friendly_name(node->get_friendly_name());
        ngraph::replace_node(node, param);
        // nGraph doesn't provide a way to set a parameter to an existing
        // function, so we clone the function here...
        func = make_shared<Function>(func->get_results(),
                                     ParameterVector{param}, func->get_name());
        auto ie_tensor = make_shared<IETensor>(element_type, shape);
        ie_tensor->write(constant->get_data_ptr(),
                         shape_size(shape) * element_type.size());
        m_hoisted_params.push_back(
            make_pair(param->get_friendly_name(), ie_tensor));
        NGRAPH_VLOG(1) << "Converted node " << constant << " to a parameter "
                       << param;
        param_replaced = true;
        break;
      }
    }
    if (!param_replaced) {
      THROW_IE_EXCEPTION
          << "Unable to add a parameter to a function with no parameters!";
    }
    auto finish_param = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> elapsed_param = finish_param - start_param;
    std::cout << "Func Parameters Check Time: " << elapsed_param.count() << " s\n";
  }

  m_function = func;

  NGRAPH_VLOG(2) << "Creating IE CNN network using nGraph function";
  auto start_net = std::chrono::high_resolution_clock::now();
  m_network = InferenceEngine::CNNNetwork(func);
  auto finish_net = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_net = finish_net - start_net;
  std::cout << "CNNNetwork Time: " << elapsed_net.count() << " s\n";

  if (std::getenv("NGRAPH_TF_DUMP_GRAPHS")) {
    auto& name = m_network.getName();
    m_network.serialize(name + ".xml", name + ".bin");
    ngraph::plot_graph(func, "tf_function_" + name + "_ie.dot");
  }

  NGRAPH_VLOG(2) << "Loading IE CNN network to device " << m_device;

  //InferenceEngine::Core ie;
  // Load network to the plugin (m_device) and create an infer request
  //auto start_load = std::chrono::high_resolution_clock::now();
  //m_exe_network = ie.LoadNetwork(m_network, m_device);
  //auto finish_load = std::chrono::high_resolution_clock::now();
  //std::chrono::duration<double> elapsed_load = finish_load - start_load;
  //std::cout << "LoadNetwork Time: " << elapsed_load.count() << " s\n";

  //auto start_net = std::chrono::high_resolution_clock::now();
  //InferenceEngine::CNNNetwork ie_network(func);
  //auto finish_net = std::chrono::high_resolution_clock::now();
  //std::chrono::duration<double> elapsed_net = finish_net - start_net;
  //std::cout << "CNNNetwork Time: " << elapsed_net.count() << " s\n";
  auto start_init_ex = std::chrono::high_resolution_clock::now();
  m_ie_executor = make_shared<IE_Executor>(m_network, m_device);
  auto finish_init_ex = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_init_ex = finish_init_ex - start_init_ex;
  std::cout << "Init Ex Time: " << elapsed_init_ex.count() << " s\n";
  m_ng_func = m_network.getFunction();
}

bool Executable::call(const vector<shared_ptr<runtime::Tensor>>& inputs,
                      vector<shared_ptr<runtime::Tensor>>& outputs,
                      bool multi_req_execution) {
  if (m_trivial_fn) {
    NGRAPH_VLOG(2) << "Calling trivial IE function with inputs="
                   << inputs.size() << " outputs=" << outputs.size();
    return call_trivial(inputs, outputs);
  }

  // Check if the number of inputs that the CNN network expects is equal to the
  // sum of the
  // inputs specified and the inputs we hoisted, if any.
  InferenceEngine::InputsDataMap input_info = m_network.getInputsInfo();
  if (input_info.size() != (inputs.size() + m_hoisted_params.size())) {
    THROW_IE_EXCEPTION
        << "Function inputs number differ from number of given inputs";
  }

  //  Prepare input blobs
  std::vector<std::shared_ptr<IE_Data>> ie_inputs(inputs.size());
  auto parameters = m_ng_func->get_parameters();
  for (int i = 0; i < inputs.size(); i++) {
    shared_ptr<IETensor> tv = static_pointer_cast<IETensor>(inputs[i]);
    ie_inputs[i] = tv->get_ie_data();
    ie_inputs[i]->set_name(parameters[i]->get_friendly_name());
  }

  std::vector<std::shared_ptr<IE_Data>> ie_hoisted_params(
      m_hoisted_params.size());
  int j = 0;
  for (const auto& it : m_hoisted_params) {
    shared_ptr<IETensor> tv = static_pointer_cast<IETensor>(it.second);
    ie_hoisted_params[j] = tv->get_ie_data();
    ie_hoisted_params[j++]->set_name(it.first);
  }

  InferenceEngine::OutputsDataMap output_info = m_network.getOutputsInfo();
  if (outputs.size() == 0 && output_info.size() > 0) {
    outputs.resize(output_info.size(), nullptr);
  }

  auto get_output_name = [](std::shared_ptr<ngraph::Node> node) {
    // Since IE has no "result" nodes, we set the blob corresponding to the
    // parent of this result node
    auto parent = node->input_value(0).get_node_shared_ptr();
    auto name = parent->get_friendly_name();
    // if parent has multiple outputs, correctly identify the output feeding
    // into this result
    if (parent->outputs().size() > 1) {
      name += "." + to_string(node->input_value(0).get_index());
    }
    return name;
  };

  //  Prepare output blobs
  std::vector<std::shared_ptr<IE_Data>> ie_outputs(outputs.size());
  auto results = m_ng_func->get_results();
  for (int i = 0; i < results.size(); i++) {
    if (outputs[i] != nullptr) {
      shared_ptr<IETensor> tv = static_pointer_cast<IETensor>(outputs[i]);
      ie_outputs[i] = tv->get_ie_data();
      ie_outputs[i]->set_name(get_output_name(results[i]));
    } else {
      ie_outputs[i] = make_shared<IE_Data>(get_output_name(results[i]));
    }
  }

  auto start_executor_inf = std::chrono::high_resolution_clock::now();
  m_ie_executor->infer(ie_inputs, ie_outputs, ie_hoisted_params,
                       multi_req_execution);
  auto finish_executor_inf = std::chrono::high_resolution_clock::now();
  std::chrono::duration<double> elapsed_executor_inf = finish_executor_inf - start_executor_inf;
  std::cout << "Executor Call Time: " << elapsed_executor_inf.count() << " s\n";

  // Set dynamic output blobs
  for (int i = 0; i < results.size(); i++) {
    if (outputs[i] == nullptr) {
      NGRAPH_VLOG(4) << "Executable::call() GetBlob()";
      outputs[i] = make_shared<IETensor>(ie_outputs[i]);
    }
  }

  return true;
}

bool Executable::call_trivial(const vector<shared_ptr<runtime::Tensor>>& inputs,
                              vector<shared_ptr<runtime::Tensor>>& outputs) {
  // outputs are in the same order as results
  auto results = m_trivial_fn->get_results();
  if (outputs.size() == 0 && results.size() > 0) {
    outputs.resize(results.size(), nullptr);
  }

  for (int i = 0; i < results.size(); i++) {
    auto& shape = results[i]->get_shape();
    if (count(shape.begin(), shape.end(), 0)) {
      if (outputs[i] == nullptr) {
        outputs[i] =
            make_shared<IETensor>(results[i]->get_element_type(), shape);
      }
      NGRAPH_VLOG(2) << "Skipping function with zero dim result...";
      continue;
    }
    auto parent = results[i]->input_value(0).get_node_shared_ptr();
    if (ngraph::is_type<opset::Parameter>(parent)) {
      NGRAPH_VLOG(2) << "Calling parameter -> result function...";
      auto param = ngraph::as_type_ptr<opset::Parameter>(parent);
      auto index = m_trivial_fn->get_parameter_index(param);
      if (index < 0) {
        THROW_IE_EXCEPTION << "Input parameter " << param->get_friendly_name()
                           << " not found in trivial function";
      }
      if (outputs[i] == nullptr) {
        outputs[i] = make_shared<IETensor>(inputs[index]->get_element_type(),
                                           inputs[index]->get_shape());
      }
      auto size = inputs[index]->get_size_in_bytes();
      unsigned char* buf_ptr = new unsigned char[size];
      inputs[index]->read(buf_ptr, size);
      outputs[i]->write(buf_ptr, size);
      delete buf_ptr;
    } else if (ngraph::is_type<opset::Constant>(parent)) {
      NGRAPH_VLOG(2) << "Calling constant -> result function...";
      auto constant = ngraph::as_type_ptr<opset::Constant>(parent);
      if (outputs[i] == nullptr) {
        outputs[i] = make_shared<IETensor>(
            constant->get_element_type(), constant->get_shape(),
            const_cast<void*>(constant->get_data_ptr()));
      } else {
        outputs[i]->write(constant->get_data_ptr(),
                          shape_size(constant->get_shape()) *
                              constant->get_element_type().size());
      }
    } else {
      THROW_IE_EXCEPTION << "Expected constant or parameter feeding to a "
                            "result in trivial function";
    }
  }
  return true;
}

size_t Executable::get_batch_size(size_t input_batch_size,
                                  std::string device) const {
  return m_ie_executor->getOutputBatchSize(input_batch_size, device);
}
}
}
