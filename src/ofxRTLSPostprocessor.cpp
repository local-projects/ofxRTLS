#include "ofxRTLSPostprocessor.h"

// --------------------------------------------------------------
ofxRTLSPostprocessor::ofxRTLSPostprocessor() {

}

// --------------------------------------------------------------
ofxRTLSPostprocessor::~ofxRTLSPostprocessor() {

	waitForThread(true);
}

// --------------------------------------------------------------
void ofxRTLSPostprocessor::setup(string _name, string _abbr, string _dictPath, 
	string _filterList) {

	name = _name;
	abbr = _abbr;
	dictPath = _dictPath;

	// Setup postprocessing params
	string ruiGroupFull = "ofxRTLSPostprocessor - " + name;
	string ruiGroupAbbr = "RTLS-P-" + abbr;
	RUI_NEW_GROUP(ruiGroupFull);
	RUI_SHARE_PARAM_WCN(ruiGroupAbbr + "- Map IDs", bMapIDs);
	RUI_SHARE_PARAM_WCN(ruiGroupAbbr + "- Remove Invalid IDs", bRemoveInvalidIDs);
	RUI_SHARE_PARAM_WCN(ruiGroupAbbr + "- Apply Filters", bApplyFilters);
	RUI_SHARE_PARAM_WCN(ruiGroupAbbr + "- ID Dict Path", dictPath);
	
	// Load the dictionary
	if (!dictPath.empty()) dict.setup(dictPath);

	// Setup the filters
	filters.setup("RTLS-"+abbr, _filterList);

	startThread();
}

// --------------------------------------------------------------
void ofxRTLSPostprocessor::threadedFunction() {

	while (isThreadRunning()) {

		// Process all data that is waiting
		while (!dataQueue.empty()) {

			// Get the next element
			mtx.lock();
			DataElem* elem = dataQueue.front();
			dataQueue.pop();
			mtx.unlock();

			// Process this element
			_process(elem->data.frame);

			// Send out this data
			ofNotifyEvent(*(elem->dataReadyEvent), elem->data);

			// Delete this data
			delete elem;
		}

		sleep(1); // ?
	}
}

// --------------------------------------------------------------
void ofxRTLSPostprocessor::exit() {

}

// --------------------------------------------------------------
void ofxRTLSPostprocessor::processAndSend(ofxRTLSEventArgs& data, 
	ofEvent<ofxRTLSEventArgs>& dataReadyEvent) {

	DataElem* elem = new DataElem();
	elem->data = data;
	elem->dataReadyEvent = &dataReadyEvent;

	mtx.lock();
	dataQueue.push(elem);
	mtx.unlock();
}

// --------------------------------------------------------------
void ofxRTLSPostprocessor::_process(RTLSProtocol::TrackableFrame& frame) {

	if (bMapIDs) {	// Map IDs if they are valid
		for (int i = 0; i < frame.trackables_size(); i++) {
			int ID = frame.trackables(i).id();
			if (ID < 0) continue;
			frame.mutable_trackables(i)->set_id(dict.lookup(ID));
		}
	}

	if (bRemoveInvalidIDs) { // Remove IDs that are invalid (< 0)
		int i = 0;
		while (i < frame.trackables_size()) {

			// Check if this trackable's ID is invalid
			if (frame.trackables(i).id() < 0) {
				// Arbitrary elements cannot be deleted. Swap this element with the last
				// and remove the last element.
				frame.mutable_trackables()->SwapElements(i, frame.trackables_size() - 1);
				frame.mutable_trackables()->RemoveLast();
			}
			else {
				i++;
			}
		}
	}

	// For all that remain in the filter, set their new coordinates and export them
	if (bApplyFilters) {
		// Input the new data
		for (int i = 0; i < frame.trackables_size(); i++) {
			// Add new data to the filter 
			glm::vec3 position = glm::vec3(
				frame.trackables(i).position().x(),
				frame.trackables(i).position().y(),
				frame.trackables(i).position().z());
			auto* filter = filters.getFilter(getFilterKey(frame.trackables(i)));
			filter->process(position);
		}

		// Process any remaining filters that haven't seen data
		filters.processRemaining();

		// Delete any data that is invalid.
		// Also save IDs for all data that is valid.
		set<string> existingDataIDs;
		int i = 0;
		while (i < frame.trackables_size()) {
			// Check if this trackable's data is invalid.
			ofxFilter* filter = filters.getFilter(getFilterKey(frame.trackables(i)));
			if (!filter->isDataValid()) {
				// If not, delete it
				frame.mutable_trackables()->SwapElements(i, frame.trackables_size() - 1);
				frame.mutable_trackables()->RemoveLast();
			}
			else {
				// Save that this ID has valid data
				existingDataIDs.insert(getFilterKey(frame.trackables(i)));

				// Set this new processed data
				glm::vec3 data = filter->getPosition();
				frame.mutable_trackables(i)->mutable_position()->set_x(data.x);
				frame.mutable_trackables(i)->mutable_position()->set_y(data.y);
				frame.mutable_trackables(i)->mutable_position()->set_z(data.z);

				i++;
			}
		}

		// Add any data that isn't present
		for (auto& it : filters.getFilters()) {
			// Check if this is a new ID and if it has valid data.
			if (existingDataIDs.find(it.first) == existingDataIDs.end() && it.second->isDataValid()) {
				// If so, add a trackable with this ID				
				Trackable* trackable = frame.add_trackables();
				if (it.first.size() == 16) {	// cuid
					trackable->set_id(-1);
					trackable->set_cuid(it.first);
				}
				else if (!it.first.empty()) {	// id
					trackable->set_id(ofToInt(it.first));
				}
				Trackable::Position* position = trackable->mutable_position();
				glm::vec3 data = it.second->getPosition();
				position->set_x(data.x);
				position->set_y(data.y);
				position->set_z(data.z);
			}
		}

		// Delete any filters that haven't been used recently
		if (ofGetElapsedTimeMillis() - lastFilterCullingTime > filterCullingPeriod) {
			lastFilterCullingTime = ofGetElapsedTimeMillis();
			filters.removeUnused();
		}
	}
}

// --------------------------------------------------------------
string ofxRTLSPostprocessor::getFilterKey(const Trackable& t) {

	// Use the ID as the key, if valid (>= 0)
	if (t.id() >= 0) return ofToString(t.id());

	// Otherwise, use the cuid
	if (!t.cuid().empty()) return t.cuid();

	// Otherwise, return an empty string
	return "";
}

// --------------------------------------------------------------