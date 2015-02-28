
/*
    Handles a filament detector that has an optical encoder wheel, that generates pulses as the filament
    moves through it.
    It also supports a "bulge" detector that triggers if the filament has a bulge in it
*/

#include "FilamentDetector.h"
#include "Kernel.h"
#include "Config.h"
#include "checksumm.h"
#include "ConfigValue.h"
#include "SlowTicker.h"
#include "PublicData.h"
#include "StreamOutputPool.h"
#include "StreamOutput.h"
#include "SerialMessage.h"
#include "FilamentDetector.h"
#include "utils.h"

#include "InterruptIn.h" // mbed
#include "us_ticker_api.h" // mbed

#define extruder_checksum CHECKSUM("extruder")

#define filament_detector_checksum  CHECKSUM("filament_detector")
#define enable_checksum             CHECKSUM("enable")
#define encoder_pin_checksum        CHECKSUM("encoder_pin")
#define bulge_pin_checksum          CHECKSUM("bulge_pin")
#define seconds_per_check_checksum  CHECKSUM("seconds_per_check")
#define pulses_per_mm_checksum      CHECKSUM("pulses_per_mm")

FilamentDetector::FilamentDetector()
{
    suspended= false;
    filament_out_alarm= false;
}

FilamentDetector::~FilamentDetector()
{
    if(encoder_pin != nullptr) delete encoder_pin;
}

void FilamentDetector::on_module_loaded()
{
    // if the module is disabled -> do nothing
    if(!THEKERNEL->config->value( filament_detector_checksum, enable_checksum )->by_default(false)->as_bool()) {
        // as this module is not needed free up the resource
        delete this;
        return;
    }

    // load settings
    Pin dummy_pin;
    dummy_pin.from_string( THEKERNEL->config->value(filament_detector_checksum, encoder_pin_checksum)->by_default("nc" )->as_string());
    this->encoder_pin= dummy_pin.interrupt_pin();
    if(this->encoder_pin == nullptr) {
        // was not a valid interrupt pin
        delete this;
        return;
    }

    this->encoder_pin->rise(this, &FilamentDetector::on_pin_rise);
    NVIC_SetPriority(EINT3_IRQn, 16); // set to low priority

    // optional bulge detector
    bulge_pin.from_string( THEKERNEL->config->value(filament_detector_checksum, bulge_pin_checksum)->by_default("nc" )->as_string())->as_input();
    if(bulge_pin.connected()) {
        // input pin polling
        THEKERNEL->slow_ticker->attach( 100, this, &FilamentDetector::button_tick);
    }

    seconds_per_check= THEKERNEL->config->value(filament_detector_checksum, seconds_per_check_checksum)->by_default(2)->as_number();
    pulses_per_mm= THEKERNEL->config->value(filament_detector_checksum, pulses_per_mm_checksum)->by_default(1)->as_number();

    // register event-handlers
    register_for_event(ON_SECOND_TICK);
    register_for_event(ON_MAIN_LOOP);
    register_for_event(ON_CONSOLE_LINE_RECEIVED);
}

void FilamentDetector::send_command(std::string msg, StreamOutput *stream)
{
    struct SerialMessage message;
    message.message = msg;
    message.stream = stream;
    THEKERNEL->call_event(ON_CONSOLE_LINE_RECEIVED, &message );
}

// needed to detect when we resume
void FilamentDetector::on_console_line_received( void *argument )
{
    if(!suspended) return;

    SerialMessage new_message = *static_cast<SerialMessage *>(argument);
    string possible_command = new_message.message;
    string cmd = shift_parameter(possible_command);
    if(cmd == "resume") {
        suspended= false;
    }
}

void FilamentDetector::on_main_loop(void *argument)
{
    if (this->filament_out_alarm) {
        this->filament_out_alarm = false;
        THEKERNEL->streams->printf("// Filament Detector has detected a filament jam\n");
        this->suspended= true;
        // fire suspend command
        this->send_command( "M600", &(StreamOutput::NullStream) );
    }
}

void FilamentDetector::on_second_tick(void *argument)
{
    if(++seconds_passed >= seconds_per_check) {
        seconds_passed= 0;
        check_encoder();
    }
}

// encoder pin interrupt
void FilamentDetector::on_pin_rise()
{
    this->pulses++;
}

void FilamentDetector::check_encoder()
{
    uint32_t pulse_cnt= this->pulses.exchange(0); // atomic load and reset
    if(suspended) return; // already suspended

    // get number of E steps taken and make sure we have seen enough pulses to cover that
    float *rd;
    if(!PublicData::get_value( extruder_checksum, (void **)&rd )) return;
    float e_moved= *(rd+5); // current position for extruder in mm
    float delta= e_moved - e_last_moved;
    e_last_moved= e_moved;

    // figure out how many pulses need to have happened to cover that e move
    uint32_t needed_pulses= floorf(delta*pulses_per_mm);
    // NOTE if needed_pulses is 0 then extruder did not move since last check, or not enough to register
    if(needed_pulses == 0) return;

    if(pulse_cnt == 0) {
        // we got no pulses and E moved since last time so fire off alarm
        this->filament_out_alarm= true;
    }
}

uint32_t FilamentDetector::button_tick(uint32_t dummy)
{
    if(!bulge_pin.connected()) return 0;

    if(bulge_pin.get()) {
        // we got a trigger from the bulge detector
        this->filament_out_alarm= true;
    }

    return 0;
}
