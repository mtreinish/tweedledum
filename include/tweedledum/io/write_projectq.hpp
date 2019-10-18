/*--------------------------------------------------------------------------------------------------
| This file is distributed under the MIT License.
| See accompanying file /LICENSE for details.
*-------------------------------------------------------------------------------------------------*/
#pragma once

#include "../gates/gate_lib.hpp"

#include <fmt/format.h>
#include <fstream>
#include <iostream>
#include <kitty/detail/mscfix.hpp>
#include <string>

namespace tweedledum {

/*! \brief Writes network in ProjecQ format into output stream
 *
 * An overloaded variant exists that writes the network into a file.
 *
 * \param network Network
 * \param os Output stream
 */
template<typename Network>
void write_projectq(Network const& network, std::ostream& os = std::cout)
{
	network.foreach_gate([&](auto const& node) {
		auto const& gate = node.gate;

		std::string controls;
		std::string negative_controls;
		gate.foreach_control([&](auto control) {
			if (!controls.empty()) {
				controls += ", ";
			}
			controls += fmt::format("qs[{}]", control.index());
			if (control.is_complemented()) {
				if (!negative_controls.empty()) {
					negative_controls += ", ";
				}
				negative_controls += fmt::format("qs[{}]", control.index());
			}
		});

		std::string targets;
		gate.foreach_target([&](auto target) {
			if (!targets.empty()) {
				targets += ", ";
			}
			targets += fmt::format("qs[{}]", target.index());
		});

		if (!negative_controls.empty()) {
			os << fmt::format("X | {}\n", negative_controls);
		}
		switch (gate.operation()) {
		default:
			std::cout << "[w] unknown gate kind \n";
			assert(false);
			break;

		case gate_lib::hadamard:
			os << fmt::format("H | {}\n", targets);
			break;

		case gate_lib::rotation_x: {
			angle rotation_angle = gate.rotation_angle();
			if (rotation_angle == angles::pi) {
				os << fmt::format("X | {}\n", targets);
			} else {
				os << fmt::format("Rx({}) | {}\n", rotation_angle.numeric_value(),
						targets);
			}
		} break;

		case gate_lib::rotation_y: {
			angle rotation_angle = gate.rotation_angle();
			if (rotation_angle == angles::pi) {
				os << fmt::format("Y | {}\n", targets);
			} else {
				os << fmt::format("Ry({}) | {}\n", rotation_angle.numeric_value(),
						targets);
			}
		} break;

		case gate_lib::rotation_z: {
			angle rotation_angle = node.gate.rotation_angle();
			if (rotation_angle == angles::pi_quarter) {
				os << fmt::format("T | {}\n", targets);
			} else if (rotation_angle == -angles::pi_quarter) {
				os << fmt::format("Tdag | {}\n", targets);
			} else if (rotation_angle == angles::pi_half) {
				os << fmt::format("S | {}\n", targets);
			} else if (rotation_angle == -angles::pi_half) {
				os << fmt::format("Sdag | {}\n", targets);
			} else if (rotation_angle == angles::pi) {
				os << fmt::format("Z | {}\n", targets);
			} else {
				os << fmt::format("Rz({}) | {}\n", rotation_angle.numeric_value(),
						targets);
			}
		} break;

		case gate_lib::cx:
			os << fmt::format("CNOT | ({}, {})\n", controls, targets);
			break;

		case gate_lib::cz:
			os << fmt::format("CZ | ({}, {})\n", controls, targets);
			break;

		case gate_lib::mcx:
			os << fmt::format("C(All(X), {}) | ([{}], [{}])\n", gate.num_controls(),
			                  controls, targets);
			break;

		case gate_lib::mcz:
			os << fmt::format("C(All(Z), {}) | ([{}], [{}])\n", gate.num_controls(),
			                  controls, targets);
			break;

		case gate_lib::swap:
			os << fmt::format("Swap | ({})\n", targets);
			break;
		}
		if (!negative_controls.empty()) {
			os << fmt::format("X | {}\n", negative_controls);
		}
	});
}

/*! \brief Writes network in ProjecQ format into a file
 *
 * \param network Network
 * \param filename Filename
 */
template<typename Network>
void write_projectq(Network const& network, std::string const& filename)
{
	std::ofstream os(filename.c_str(), std::ofstream::out);
	write_projectq(network, os);
}

} // namespace tweedledum
