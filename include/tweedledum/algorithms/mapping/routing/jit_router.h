/*------------------------------------------------------------------------------
| Part of the tweedledum.  This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
*-----------------------------------------------------------------------------*/
#pragma once

#include "../../../ir/Circuit.h"
#include "../../../ir/Gate.h"
#include "../../../ir/MappedDAG.h"
#include "../../../ir/Node.h"
#include "../../../ir/Wire.h"
#include "../../../target/Device.h"
#include "../../utility/reverse.h"

#include <vector>

namespace tweedledum {

/*! \brief Configuration parameters for `jit_router`. */
struct jit_config {
	uint32_t e_set_size = 20;
	float e_weight = 0.5;
	float decay_delta = 0.001;
	uint32_t num_rounds_decay_reset = 5;
	bool randomize_initial_map = false;
	bool use_look_ahead = true;
};

#pragma region Implementation details
namespace detail {

class jit_router {
	using swap_type = std::pair<uint32_t, uint32_t>;
	using node_type = typename Circuit::node_type;
	using op_type = typename Circuit::op_type;

public:
	jit_router(Device const& device, jit_config const& parameters)
	    : device_(device), original_(nullptr), mapped_(nullptr),
	      config_(parameters), involved_phy_(device.num_qubits(), 0u),
	      phy_decay_(device_.num_qubits(), 1.0),
	      unexectued_(device.num_qubits())
	{}

	MappedDAG route(Circuit const& original,
	    std::vector<wire::Id> const& placement, bool finalize = true)
	{
		assert(placement.size() == device_.num_qubits());
		reset();
		MappedDAG mapped(original, device_);
		original_ = &original;
		mapped_ = &mapped;
		new_mapping(placement);

		// fmt::print("Sabre rounting: begin.\n");
		original_->clear_values();
		original_->foreach_output([&](node_type const& node,
		                              node::Id const id) {
			if (node.op.is_meta()) {
				return;
			}
			if (original_->incr_value(node) == node.op.num_wires())
			{
				front_layer_.push_back(id);
			}
		});

		uint32_t num_swap_searches = 0u;
		while (!front_layer_.empty()) {
			// print_front_layer();
			if (!try_add_front_layer()) {
				num_swap_searches += 1;
				auto [phy0, phy1] = find_swap();
				if ((num_swap_searches
				        % config_.num_rounds_decay_reset)
				    == 0) {
					std::fill(phy_decay_.begin(),
					    phy_decay_.end(), 1.0);
				} else {
					phy_decay_.at(phy0)
					    += config_.decay_delta;
					phy_decay_.at(phy1)
					    += config_.decay_delta;
				}
				add_swap(wire::make_qubit(phy0),
				    wire::make_qubit(phy1));
				std::fill(involved_phy_.begin(),
				    involved_phy_.end(), 0);
			}
		}
		// fmt::print("Sabre rounting: end.\n");
		// print_v_to_phy();
		// print_phy_to_v();

		if (finalize) {
			std::vector<wire::Id> free_phy = find_free_phy();
			for (uint32_t i = 0; i < v_to_phy_.size(); ++i) {
				if (v_to_phy_.at(i) == wire::invalid_id) {
					assert(!free_phy.empty());
					wire::Id phy = free_phy.back();
					free_phy.pop_back();
					v_to_phy_.at(i) = phy;
					jit_add(wire::make_qubit(i));
				}
			}
		}
		// Set final placement
		mapped_->v_to_phy(v_to_phy_);
		return mapped;
	}

private:
	void reset()
	{
		wire_to_v_.clear();
		phy_to_v_.clear();
		std::fill(phy_decay_.begin(), phy_decay_.end(), 1.0);
	}

	void new_mapping(std::vector<wire::Id> const& placement)
	{
		// original circuit wire -> mapped virtual qubit
		wire_to_v_.resize(original_->num_wires(), wire::invalid_id);
		original_->foreach_wire(
		    [&](wire::Id wire, std::string_view name) {
			    wire_to_v_.at(wire) = mapped_->wire(name);
		    });

		// Set initial placement: mapped virtual qubit -> mapped
		// physical qubit
		v_to_phy_ = placement;
		mapped_->v_to_phy(v_to_phy_);
		// Placement: mapped physical qubit -> mapped virtual qubit
		phy_to_v_.resize(device_.num_qubits(), wire::invalid_id);
		for (uint32_t i = 0; i < v_to_phy_.size(); ++i) {
			if (v_to_phy_.at(i) == wire::invalid_id) {
				continue;
			}
			phy_to_v_.at(v_to_phy_.at(i)) = wire::make_qubit(i);
		}
	}

private:
	wire::Id wire_to_phy(wire::Id const w0) const
	{
		return v_to_phy_.at(wire_to_v_.at(w0));
	}

	void jit_add(wire::Id const v)
	{
		assert(v < unexectued_.size());
		for (Gate const* g : unexectued_.at(v)) {
			wire::Id const phy = v_to_phy_.at(v);
			mapped_->create_op(*g, phy);
		}
		unexectued_.at(v).clear();
	}

	// One-qubit operations can always be mapped
	bool add_op(Gate const& g, wire::Id const w0)
	{
		wire::Id const phy0 = wire_to_phy(w0);
		if (phy0 == wire::invalid_id) {
			unexectued_.at(wire_to_v_.at(w0)).emplace_back(&g);
		} else {
			mapped_->create_op(g, phy0);
		}
		return true;
	}

	void add_swap(wire::Id const phy0, wire::Id const phy1)
	{
		assert(device_.are_connected(phy0, phy1));
		wire::Id const v0 = phy_to_v_.at(phy0);
		wire::Id const v1 = phy_to_v_.at(phy1);
		if (v0 != wire::invalid_id) {
			v_to_phy_.at(v0) = phy1;
		}
		if (v1 != wire::invalid_id) {
			v_to_phy_.at(v1) = phy0;
		}
		std::swap(phy_to_v_.at(phy0), phy_to_v_.at(phy1));
		mapped_->create_op(GateLib::swap, phy0, phy1);
	}

	std::vector<wire::Id> find_free_phy() const
	{
		std::vector<wire::Id> free_phy;
		for (uint32_t i = 0; i < phy_to_v_.size(); ++i) {
			if (phy_to_v_.at(i) == wire::invalid_id) {
				free_phy.emplace_back(i, true);
			}
		}
		return free_phy;
	}

	void place_two_v(wire::Id const v0, wire::Id const v1)
	{
		wire::Id phy0 = v_to_phy_.at(v0);
		wire::Id phy1 = v_to_phy_.at(v1);
		std::vector<wire::Id> const free_phy = find_free_phy();
		assert(free_phy.size() >= 2u);
		if (free_phy.size() == 2u) {
			phy0 = free_phy.at(0);
			phy1 = free_phy.at(1);
		} else {
			uint32_t min_dist
			    = std::numeric_limits<uint32_t>::max();
			for (uint32_t i = 0u; i < free_phy.size(); ++i) {
				for (uint32_t j = i + 1u; j < free_phy.size();
				     ++j) {
					wire::Id const i_phy = free_phy.at(i);
					wire::Id const j_phy = free_phy.at(j);
					if (min_dist
					    < device_.distance(i_phy, j_phy)) {
						continue;
					}
					min_dist
					    = device_.distance(i_phy, j_phy);
					phy0 = i_phy;
					phy1 = j_phy;
				}
			}
		}
		v_to_phy_.at(v0) = phy0;
		v_to_phy_.at(v1) = phy1;
		phy_to_v_.at(phy0) = v0;
		phy_to_v_.at(phy1) = v1;
		jit_add(v0);
		jit_add(v1);
	}

	void place_one_v(wire::Id v0, wire::Id v1)
	{
		wire::Id phy0 = v_to_phy_.at(v0);
		wire::Id phy1 = v_to_phy_.at(v1);
		std::vector<wire::Id> const free_phy = find_free_phy();
		assert(free_phy.size() >= 1u);
		if (phy1 == wire::invalid_id) {
			std::swap(v0, v1);
			std::swap(phy0, phy1);
		}
		phy0 = free_phy.at(0);
		uint32_t min_dist = device_.distance(phy1, phy0);
		for (uint32_t i = 1u; i < free_phy.size(); ++i) {
			if (min_dist > device_.distance(phy1, free_phy.at(i))) {
				min_dist
				    = device_.distance(phy1, free_phy.at(i));
				phy0 = free_phy.at(i);
			}
		}
		v_to_phy_.at(v0) = phy0;
		phy_to_v_.at(phy0) = v0;
		jit_add(v0);
	}

	bool try_add_op(Gate const& g, wire::Id const w0, wire::Id const w1)
	{
		wire::Id phy0 = wire_to_phy(w0);
		wire::Id phy1 = wire_to_phy(w1);
		if (phy0 == wire::invalid_id && phy1 == wire::invalid_id) {
			place_two_v(wire_to_v_.at(w0), wire_to_v_.at(w1));
			phy0 = wire_to_phy(w0);
			phy1 = wire_to_phy(w1);
		} else if (phy0 == wire::invalid_id || phy1 == wire::invalid_id)
		{
			place_one_v(wire_to_v_.at(w0), wire_to_v_.at(w1));
			phy0 = wire_to_phy(w0);
			phy1 = wire_to_phy(w1);
		}
		if (!device_.are_connected(phy0, phy1)) {
			return false;
		}
		if (w0.is_complemented()) {
			phy0.complement();
		}
		return mapped_->create_op(g, phy0, phy1) != node::invalid_id;
	}

private:
	// Return true if a gate of the front_layer was executed, false
	// otherwise
	bool try_add_front_layer()
	{
		bool executed = false;
		std::vector<node::Id> new_front_layer;
		for (node::Id n_id : front_layer_) {
			node_type const& node = original_->node(n_id);
			op_type const& op = node.op;
			if (op.is_meta()) {
				continue;
			}
			if (op.is_one_qubit()) {
				add_op(op, op.target());
			} else if (!try_add_op(op, op.control(), op.target())) {
				new_front_layer.push_back(n_id);
				involved_phy_.at(wire_to_phy(op.control()))
				    = 1u;
				involved_phy_.at(wire_to_phy(op.target())) = 1u;
				continue;
			}
			executed = true;
			original_->foreach_child(node,
			    [&](node_type const& child, node::Id child_id) {
				    if (child.op.is_meta()) {
					    return;
				    }
				    if (original_->incr_value(child)
				        == child.op.num_wires()) {
					    new_front_layer.push_back(child_id);
				    }
			    });
		}
		front_layer_ = new_front_layer;
		return executed;
	}

	swap_type find_swap()
	{
		// Obtain SWAP candidates
		std::vector<swap_type> swap_candidates;
		for (uint32_t i = 0u; i < device_.num_edges(); ++i) {
			auto const& [u, v] = device_.edge(i);
			if (involved_phy_.at(u) || involved_phy_.at(v)) {
				swap_candidates.emplace_back(u, v);
			}
		}

		if (config_.use_look_ahead) {
			select_extended_layer();
		}

		// Compute cost
		std::vector<double> cost;
		for (auto& [phy0, phy1] : swap_candidates) {
			std::vector<wire::Id> tmp_v_to_phy = v_to_phy_;
			wire::Id const v0 = phy_to_v_.at(phy0);
			wire::Id const v1 = phy_to_v_.at(phy1);
			if (v0 != wire::invalid_id) {
				tmp_v_to_phy.at(v0) = wire::make_qubit(phy1);
			}
			if (v1 != wire::invalid_id) {
				tmp_v_to_phy.at(v1) = wire::make_qubit(phy0);
			}
			double swap_cost
			    = compute_cost(tmp_v_to_phy, front_layer_);
			double const max_decay = std::max(
			    phy_decay_.at(phy0), phy_decay_.at(phy1));

			if (!extended_layer_.empty()) {
				double const f_cost
				    = swap_cost / front_layer_.size();
				double e_cost = compute_cost(
				    tmp_v_to_phy, extended_layer_);
				e_cost = e_cost / extended_layer_.size();
				swap_cost
				    = f_cost + (config_.e_weight * e_cost);
			}
			cost.emplace_back(max_decay * swap_cost);
		}

		// Find and return the swap with minimal cost
		uint32_t min = 0u;
		for (uint32_t i = 1u; i < cost.size(); ++i) {
			if (cost.at(i) < cost.at(min)) {
				min = i;
			}
		}
		// print_swap_candidates(swap_candidates, cost);
		return swap_candidates.at(min);
	}

	double compute_cost(std::vector<wire::Id> const& tmp_v_to_phy,
	    std::vector<node::Id> const& gates)
	{
		double cost = 0.0;
		for (node::Id n_id : gates) {
			op_type const& op = original_->node(n_id).op;
			wire::Id const phy0
			    = tmp_v_to_phy.at(wire_to_v_.at(op.control()));
			wire::Id const phy1
			    = tmp_v_to_phy.at(wire_to_v_.at(op.target()));
			if (phy0 == wire::invalid_id
			    || phy1 == wire::invalid_id) {
				continue;
			}
			cost += (device_.distance(phy0, phy1) - 1);
		}
		return cost;
	}

	void select_extended_layer()
	{
		extended_layer_.clear();
		std::vector<node::Id> incremented_nodes;
		std::vector<node::Id> tmp_front_layer = front_layer_;
		while (!tmp_front_layer.empty()) {
			std::vector<node::Id> new_tmp_front_layer;
			for (node::Id n_id : tmp_front_layer) {
				node_type const& node = original_->node(n_id);
				original_->foreach_child(node,
				    [&](node_type const& child, node::Id c_id) {
					    if (child.op.is_meta()) {
						    return;
					    }
					    incremented_nodes.emplace_back(c_id);
					    if (original_->incr_value(child)
					        == child.op.num_wires()) {
						    new_tmp_front_layer
						        .emplace_back(c_id);
						    if (!child.op.is_two_qubit())
						    {
							    return;
						    }
						    extended_layer_.emplace_back(
						        c_id);
					    }
				    });
				if (extended_layer_.size() >= config_.e_set_size)
				{
					goto undo_increment;
				}
			}
			tmp_front_layer = new_tmp_front_layer;
		}
undo_increment:
		for (node::Id n_id : incremented_nodes) {
			node_type const& node = original_->node(n_id);
			original_->decr_value(node);
		}
	}

#pragma region Debugging
private:
	void print_front_layer() const
	{
		fmt::print("front layer: [{}] {{", front_layer_.size());
		for (node::Id const& n_id : front_layer_) {
			fmt::print(" {}", n_id);
		}
		fmt::print(" }}\n");
	}

	void print_v_to_phy() const
	{
		fmt::print("v_to_phy: [{}] {{", v_to_phy_.size());
		for (wire::Id const& phy : v_to_phy_) {
			fmt::print(" {}", phy);
		}
		fmt::print(" }}\n");
	}

	void print_phy_to_v() const
	{
		fmt::print("phy_to_v: [{}] {{", phy_to_v_.size());
		for (wire::Id const& v : phy_to_v_) {
			fmt::print(" {}", v);
		}
		fmt::print(" }}\n");
	}

	void print_swap_candidates(std::vector<swap_type> const& swap_candidates,
	    std::vector<double> const& costs) const
	{
		fmt::print("swap candidates: [{}] {{\n", swap_candidates.size());
		for (uint32_t i = 0; i < swap_candidates.size(); ++i) {
			auto const& [phy0, phy1] = swap_candidates.at(i);
			fmt::print(
			    "    {} : {} [{}]\n", phy0, phy1, costs.at(i));
		}
		fmt::print("}}\n");
	}
#pragma endregion

private:
	Device const& device_;
	Circuit const* original_;
	MappedDAG* mapped_;

	jit_config config_;
	std::vector<node::Id> front_layer_;
	std::vector<node::Id> extended_layer_;
	std::vector<uint32_t> involved_phy_;
	std::vector<float> phy_decay_;
	std::vector<std::vector<Gate const*>> unexectued_;

	// Placement info
	std::vector<wire::Id> wire_to_v_;
	std::vector<wire::Id> v_to_phy_;
	std::vector<wire::Id> phy_to_v_;
};

} // namespace detail
#pragma endregion

} // namespace tweedledum
