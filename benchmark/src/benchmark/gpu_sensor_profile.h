#ifndef HEAVENBENCH_SRC_BENCHMARK_GPU_SENSOR_PROFILE_H_
#define HEAVENBENCH_SRC_BENCHMARK_GPU_SENSOR_PROFILE_H_

namespace heaven::profile {

class Recorder;
struct GpuSensorReading;

// Call on the recorder's control thread once for each newly cached sensor
// snapshot. This imports host observations without GPU/frame synchronization;
// unavailable measurements are represented by availability counters only.
void record_gpu_sensor_sample(Recorder& recorder, const GpuSensorReading& reading);

} // namespace heaven::profile
#endif
