#ifndef MIDIPLAYER_H
#define MIDIPLAYER_H

#include <libremidi/libremidi.hpp>
#include "MIDIFile.h"
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <string>
#include <condition_variable>

class MIDIPlayer {
public:
	MIDIPlayer();
	~MIDIPlayer();

	// Output device management
	static const std::vector<std::string>& availablePorts(bool force = false);
	bool connectDevice(int port);
	void disconnectDevice();
	
	// Playback control
	void loadFile(const MIDIFile* file);
	void play();
	void pause();
	void stop();
	void seek(double time);

	// Live Thru
	void sendEvent(const libremidi::message& msg);
	
	// Properties
	const std::string& deviceName() const { return _deviceName; }
	bool isConnected() const { return _midiOut != nullptr && _selectedPort >= 0; }

private:
	struct PlayableEvent {
		double time;
		libremidi::message message;
		
		bool operator<(const PlayableEvent& other) const {
			return time < other.time;
		}
	};

	void buildPlayableEvents(const MIDIFile* file);
	void threadLoop();

	static libremidi::midi_out& shared();
	static libremidi::midi_out* _sharedMIDIOut;
	static std::vector<std::string> _availablePorts;
	
	std::thread _thread;
	std::mutex _mutex;
	std::condition_variable _cv;
	
	std::vector<PlayableEvent> _events;
	size_t _playIndex = 0;
	
	std::atomic<bool> _running{true};
	std::atomic<bool> _playing{false};
	std::atomic<double> _currentTime{0.0};
	
	libremidi::midi_out* _midiOut = nullptr;
	std::string _deviceName;
	int _selectedPort = -1;

	// Precision timing
	std::chrono::time_point<std::chrono::high_resolution_clock> _playStartTime;
};

#endif // MIDIPLAYER_H
