#include <time.h>
#include "gptl.h"
#include "GPTLTimer.h"


using namespace xolotlPerf;


// This operations starts the ITimer.
void GPTLTimer::start(){
//	GPTLstart_handle(timerName.c_str(), &handle);
	GPTLstart(this->getName().c_str());
}

// This operation stops the ITimer.
void GPTLTimer::stop(){
//	GPTLstop_handle(name.c_str(), &handle);
	GPTLstop(this->getName().c_str());
}


// This operation returns the value of the ITimer.
double GPTLTimer::getValue() const {

	/*
	** GPTLget_wallclock: return wallclock accumulation for a timer.
	**
	** Input args:
	** const char *timername: timer name
	** int t: thread number (if < 0, the request is for the current thread)
	**
	** Output args:
	** double *value: current wallclock accumulation for the timer
	*/
//    int gret = GPTLget_wallclock( name.c_str(), -1, &value );
    double value = -1.0;
    GPTLget_wallclock( this->getName().c_str(), -1, &value );

    return value;
}

// This operation returns the units of the ITimer.
long GPTLTimer::getUnits() const {
//	return this->units;
}
