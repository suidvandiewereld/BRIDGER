#pragma once

extern "C" struct FreecamApi {
    unsigned version;
    bool (*enabled)();
    void (*set_enabled)(bool value);
    void (*position)(double* xyz);
    void (*angles)(float* heading_degrees, float* pitch_degrees);
    void (*teleport)(double x, double y, double z);
    float (*speed)();
    void (*set_speed)(float metres_per_second);
    bool (*first_person)();
    void (*set_first_person)(bool value);
};
