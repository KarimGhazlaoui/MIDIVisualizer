#include "MIDIPlayer.h"
#include "../rendering/State.h"
#include <iostream>
#include <algorithm>

libremidi::midi_out* MIDIPlayer::_sharedMIDIOut = nullptr;
std::vector<std::string> MIDIPlayer::_availablePorts;

MIDIPlayer::MIDIPlayer() {
}

MIDIPlayer::~MIDIPlayer() {
	stop();
	disconnectDevice();
}

libremidi::midi_out& MIDIPlayer::shared() {
	if(!_sharedMIDIOut) {
		_sharedMIDIOut = new libremidi::midi_out(libremidi::API::UNSPECIFIED, "MIDIVisualizer");
	}
	return *_sharedMIDIOut;
}

const std::vector<std::string>& MIDIPlayer::availablePorts(bool force) {
	if(_availablePorts.empty() || force) {
		const int portCount = shared().get_port_count();
		_availablePorts.resize(portCount);
		for(int i = 0; i < portCount; ++i) {
			_availablePorts[i] = shared().get_port_name(i);
		}
	}
	return _availablePorts;
}

bool MIDIPlayer::connectDevice(int port) {
	disconnectDevice();
	
	if(port < 0) {
		return false;
	}

	const auto& ports = availablePorts();
	if(port >= ports.size()) {
		std::cerr << "[MIDI Out] Invalid port index." << std::endl;
		return false;
	}

	try {
		shared().open_port(port);
		_midiOut = _sharedMIDIOut;
		_deviceName = ports[port];
		_selectedPort = port;
		return true;
	} catch(const std::exception& e) {
		std::cerr << "[MIDI Out] Failed to open port: " << e.what() << std::endl;
		return false;
	}
}

void MIDIPlayer::disconnectDevice() {
	if(_midiOut) {
		try {
			_midiOut->close_port();
		} catch(...) {}
		_midiOut = nullptr;
	}
	_deviceName = "";
	_selectedPort = -1;
}

void MIDIPlayer::loadFile(const MIDIFile* file) {
	std::lock_guard<std::mutex> lock(_mutex);
	buildPlayableEvents(file);
	_playIndex = 0;
	_currentTime = 0.0;
}

void MIDIPlayer::buildPlayableEvents(const MIDIFile* file) {
	_events.clear();
	if(!file) return;

	FilterOptions noFilter;
	noFilter.tracks.resize(file->tracksCount(), true);
	
	for(size_t track = 0; track < file->tracksCount(); ++track) {
		std::vector<MIDINote> notes;
		file->getRawNotes(notes, noFilter, track);
		
		for(const auto& note : notes) {
			// Note On
			_events.push_back({
				note.start,
				libremidi::message::note_on(note.channel + 1, note.note, note.velocity)
			});
			// Note Off
			_events.push_back({
				note.start + note.duration,
				libremidi::message::note_off(note.channel + 1, note.note, 0)
			});
		}

		std::vector<MIDIPedal> pedals;
		file->getPedals(pedals, track);
		for(const auto& pedal : pedals) {
			uint8_t control = 0;
			if(pedal.type == PedalType::DAMPER) control = 64;
			else if(pedal.type == PedalType::SOSTENUTO) control = 66;
			else if(pedal.type == PedalType::SOFT) control = 67;
			else if(pedal.type == PedalType::EXPRESSION) control = 11;
			
			if(control != 0) {
				int vel = int(pedal.velocity * 127.0f);
				if (vel < 0) vel = 0;
				if (vel > 127) vel = 127;
				// Pedal On (send on channel 1 for now)
				_events.push_back({
					pedal.start,
					libremidi::message::control_change(1, control, vel)
				});
				// Pedal Off
				_events.push_back({
					pedal.start + pedal.duration,
					libremidi::message::control_change(1, control, 0)
				});
			}
		}
	}

	std::sort(_events.begin(), _events.end());
}

void MIDIPlayer::play() {
	if(_playing.load()) return;
	
	_playing = true;
	_running = true;
	_playStartTime = std::chrono::high_resolution_clock::now() - std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(_currentTime.load()));
	
	if(!_thread.joinable()) {
		_thread = std::thread(&MIDIPlayer::threadLoop, this);
	}
	_cv.notify_one();
}

void MIDIPlayer::pause() {
	_playing = false;
	_cv.notify_one();
	
	// Send All Notes Off? Not strictly necessary unless we paused mid-note,
	// but might prevent hanging notes.
}

void MIDIPlayer::stop() {
	_running = false;
	_playing = false;
	_cv.notify_one();
	if(_thread.joinable()) {
		_thread.join();
	}
}

void MIDIPlayer::seek(double time) {
	std::lock_guard<std::mutex> lock(_mutex);
	_currentTime = time;
	_playStartTime = std::chrono::high_resolution_clock::now() - std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::duration<double>(time));
	
	// Find the next event to play
	_playIndex = 0;
	while(_playIndex < _events.size() && _events[_playIndex].time < time) {
		_playIndex++;
	}
}

void MIDIPlayer::sendEvent(const libremidi::message& msg) {
	if(_midiOut) {
		_midiOut->send_message(msg);
	}
}

void MIDIPlayer::threadLoop() {
	while(_running.load()) {
		std::unique_lock<std::mutex> lock(_mutex);
		_cv.wait(lock, [this]() { return (_playing.load() && _running.load()) || !_running.load(); });
		
		if(!_running.load()) break;

		if(_playing.load()) {
			auto now = std::chrono::high_resolution_clock::now();
			double current_time = std::chrono::duration<double>(now - _playStartTime).count();
			_currentTime = current_time;

			while(_playIndex < _events.size() && _events[_playIndex].time <= current_time) {
				if(_midiOut) {
					_midiOut->send_message(_events[_playIndex].message);
				}
				_playIndex++;
			}

			// Determine sleep time
			if(_playIndex < _events.size()) {
				double wait_time = _events[_playIndex].time - current_time;
				if(wait_time > 0) {
					lock.unlock();
					// Sleep for a short amount to allow responsive pausing/seeking
					int sleep_ms = int(wait_time * 1000);
					if (sleep_ms > 10) sleep_ms = 10;
					if (sleep_ms < 0) sleep_ms = 0;
					std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
					lock.lock();
				}
			} else {
				// Finished playing all events
				lock.unlock();
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
				lock.lock();
			}
		}
	}
}
