#include "./ledbat.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <cstdint>
#include <cassert>

#include <iomanip>
#include <iostream>
#include <limits>

LEDBAT::LEDBAT(void) {
	_time_start_offset = clock::now();

	{ // add some high delay values
		// spec want +inf
		//_rtt_buffer.push_back(_base_delay);
		//_rtt_buffer.push_back(_base_delay);
		//_rtt_buffer.push_back(_base_delay);
	}
}

size_t LEDBAT::canSend(void) const {
	if (_in_flight.empty()) {
		return 496u;
	}

	//const float time_since_last_sent {std::min(
		//getTimeNow() - std::get<1>(_in_flight.back()),
		//0.01f // 10ms max
	//)};

	//const float bps {std::min(
		//(_cwnd / getCurrentDelay()),
		//max_byterate_allowed
	//)};

	const int64_t cspace = _cwnd - _in_flight_bytes;
	if (cspace < 496) {
		return 0u;
	}

	const int64_t fspace = _fwnd - _in_flight_bytes;
	if (fspace < 496) {
		return 0u;
	}

	size_t space = std::ceil(std::min(cspace, fspace) / 496.f) * 496.f;

	// data size, no overhead
	//const int64_t can_send_size {std::min<int64_t>(
		//bps * time_since_last_sent - segment_overhead,
		//maximum_segment_size - segment_overhead
	//)};
	//const int64_t can_send_size {static_cast<int64_t>(bps * time_since_last_sent - segment_overhead)};

	//if (can_send_size < 100) {
		//return 0;
	//} else {
		//return can_send_size;
	//}

	return space;
}

void LEDBAT::onSent(SeqIDType seq, size_t data_size) {
	if (true) {
		for (const auto& it : _in_flight) {
			assert(std::get<0>(it) != seq);
		}
	}
	_in_flight.push_back({seq, getTimeNow(), data_size + segment_overhead});
	_in_flight_bytes += data_size + segment_overhead;
	_recently_sent_bytes += data_size + segment_overhead;
}

void LEDBAT::onAck(std::vector<SeqIDType> seqs) {
	// only take the smallest value
	float most_recent {-std::numeric_limits<float>::infinity()};

	int64_t acked_data {0};

	const auto now {getTimeNow()};

	for (const auto& seq : seqs) {
		auto it = std::find_if(_in_flight.begin(), _in_flight.end(), [seq](const auto& v) -> bool {
			return std::get<0>(v) == seq;
		});

		if (it == _in_flight.end()) {
			continue; // not found, ignore
		} else {
			addRTT(now - std::get<1>(*it));

			// TODO: remove
			most_recent = std::max(most_recent, std::get<1>(*it));
			_in_flight_bytes -= std::get<2>(*it);
			_recently_acked_data += std::get<2>(*it);
			assert(_in_flight_bytes >= 0);
			_in_flight.erase(it);
		}
	}

	if (most_recent == -std::numeric_limits<float>::infinity()) {
		return; // not found, ignore
	}


	//addRTT(now - most_recent);

	updateWindows();

	// update cto - no? we dont handle timeouts
}

void LEDBAT::onLoss(SeqIDType seq, bool discard) {
	auto it = std::find_if(_in_flight.begin(), _in_flight.end(), [seq](const auto& v) -> bool {
		return std::get<0>(v) == seq;
	});

	if (it == _in_flight.end()) {
		// error
		return; // not found, ignore ??
	}

	_recently_lost_data = true;

	// at most once per rtt?

	if (false) {
		std::cerr << "CCA: onLoss: TIME: " << getTimeNow() << "\n";
	}

	// TODO: "if data lost is not to be retransmitted"
	if (discard) {
		_in_flight_bytes -= std::get<2>(*it);
		assert(_in_flight_bytes >= 0);
	}

	updateWindows();
}

float LEDBAT::getCurrentDelay(void) const {
	float sum {0.f};
	size_t count {0};
	for (size_t i = 0; i < _tmp_rtt_buffer.size(); i++) {
		//sum += _tmp_rtt_buffer.at(_tmp_rtt_buffer.size()-(1+i));
		sum += _tmp_rtt_buffer.at(i);
		count++;
	}

	if (count) {
		return sum / count;
	} else {
		return std::numeric_limits<float>::infinity();
	}
}

void LEDBAT::addRTT(float new_delay) {
	auto now = getTimeNow();

	_base_delay = std::min(_base_delay, new_delay);
	// TODO: use fixed size instead? allocations can ruin perf
	_rtt_buffer.push_back({now, new_delay});

	_tmp_rtt_buffer.push_front(new_delay);
	// HACKY
	if (_tmp_rtt_buffer.size() > current_delay_filter_window) {
		_tmp_rtt_buffer.resize(current_delay_filter_window);
	}

	// is it 1 minute yet
	if (now - _rtt_buffer.front().first >= 30.f) {

		float new_section_minimum = new_delay;
		for (const auto it : _rtt_buffer) {
			new_section_minimum = std::min(it.second, new_section_minimum);
		}

		_rtt_buffer_minutes.push_back(new_section_minimum);

		_rtt_buffer.clear();

		if (_rtt_buffer_minutes.size() > 20) {
			_rtt_buffer_minutes.pop_front();
		}

		_base_delay = std::numeric_limits<float>::infinity();
		for (const float it : _rtt_buffer_minutes) {
			_base_delay = std::min(_base_delay, it);
		}
	}
}

void LEDBAT::updateWindows(void) {
	const auto now {getTimeNow()};

	const float current_delay {getCurrentDelay()};

	if (now - _last_cwnd >= current_delay) {
		const float queuing_delay {current_delay - _base_delay};

		_fwnd = max_byterate_allowed * getCurrentDelay();
		_fwnd *= 1.3f; // try do balance conservative algo a bit, current_delay

		//const float gain {1}; // TODO: move and increase
		float gain {1.f / std::min(16.f, std::ceil(2.f*target_delay/_base_delay))};
		//gain *= 400.f; // from packets to bytes ~
		gain *= _recently_acked_data/10.f; // from packets to bytes ~
		//gain *= 0.1f;

		if (_recently_lost_data) {
			_cwnd = std::clamp(
				_cwnd / 2.f,
				2.f * maximum_segment_size,
				_cwnd
			);
		} else {
			// LEDBAT++ (the Rethinking the LEDBAT Protocol paper)
			// "Multiplicative decrease"
			const float constant {2.f}; // spec recs 1
			if (queuing_delay < target_delay) {
				_cwnd += gain;
				_cwnd = std::min(
					_cwnd + gain,
					_fwnd
				);
			} else if (queuing_delay > target_delay) {
				_cwnd = std::clamp(
					_cwnd + std::max( // TODO: where to put bytes_newly_acked
						gain - constant * _cwnd * (queuing_delay / target_delay - 1.f),
						-_cwnd/2.f // at most halve
					),

					// never drop below 2 "packets" in flight
					//2.f * maximum_segment_size,
					2.f * 496,

					current_delay * max_byterate_allowed // cap rate
				);
			} // no else, we on point. very unlikely with float
		}

		if (false) { // plotting
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " cwnd: " << _cwnd << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " fwnd: " << _fwnd << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " current_delay: " << current_delay << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " base_delay: " << _base_delay << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " gain: " << gain << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " speed: " << (_recently_sent_bytes / (now - _last_cwnd)) / (1024*1024) << "\n";
			std::cerr << std::fixed << "CCA: onAck: TIME: " << now << " in_flight_bytes: " << _in_flight_bytes << "\n";
		}

		_last_cwnd = now;
		_recently_acked_data = 0;
		_recently_lost_data = false;
		_recently_sent_bytes = 0;
	}
}

