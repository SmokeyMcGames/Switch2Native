#include <windows.h>
#include <wdf.h>
#include <initguid.h>
#include <SensorsCx.h>
#include <SensorsUtils.h>
#include <propvarutil.h>
#include "motion_reader.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace {
template <size_t Count> class Collection {
  public:
    Collection() {
        SENSOR_COLLECTION_LIST_INIT(get(), ULONG(storage_.size()));
        get()->Count = Count;
    }
    ~Collection() {
        for (auto &item : entries()) PropVariantClear(&item.Value);
    }
    Collection(const Collection &) = delete;
    Collection &operator=(const Collection &) = delete;
    PSENSOR_COLLECTION_LIST get() { return reinterpret_cast<PSENSOR_COLLECTION_LIST>(storage_.data()); }
    auto entries() { return std::span(get()->List, Count); }
    auto &operator[](size_t i) { return get()->List[i]; }
    NTSTATUS copy(PSENSOR_COLLECTION_LIST destination, PULONG size) {
        if (!size) return STATUS_INVALID_PARAMETER;
        *size = CollectionsListGetMarshalledSize(get());
        if (!destination) return STATUS_SUCCESS;
        if (destination->AllocatedSizeInBytes < *size) return STATUS_BUFFER_TOO_SMALL;
        return CollectionsListCopyAndMarshall(destination, get());
    }
  private:
    alignas(SENSOR_COLLECTION_LIST) std::array<std::byte, offsetof(SENSOR_COLLECTION_LIST, List) + Count * sizeof(SENSOR_VALUE_PAIR)> storage_{};
};

const std::array<PROPERTYKEY, 3> gyro_keys{
    PKEY_SensorData_AngularVelocityX_DegreesPerSecond,
    PKEY_SensorData_AngularVelocityY_DegreesPerSecond,
    PKEY_SensorData_AngularVelocityZ_DegreesPerSecond};
const std::array<PROPERTYKEY, 3> accel_keys{
    PKEY_SensorData_AccelerationX_Gs, PKEY_SensorData_AccelerationY_Gs, PKEY_SensorData_AccelerationZ_Gs};

struct SensorState {
    SRWLOCK lock = SRWLOCK_INIT;
    bool gyro{}, started{}, first{true};
    ULONG interval{4};
    SENSOR_STATE state{SensorState_Idle};
    std::array<float, 3> thresholds{}, last_axes{};
    uint64_t last_timestamp{};
    Collection<6> enumeration;
    Collection<4> data;
    const auto &keys() const { return gyro ? gyro_keys : accel_keys; }
    GUID type() const { return gyro ? GUID_SensorType_Gyrometer3D : GUID_SensorType_Accelerometer3D; }
};
struct SensorContext { SensorState *state; };
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SensorContext, sensor_context)
struct DeviceContext {
    unsigned slot;
    SENSOROBJECT sensors[2];
    MotionReader *reader;
};
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DeviceContext, device_context)

struct Lock {
    explicit Lock(SRWLOCK &lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~Lock() { ReleaseSRWLockExclusive(&lock_); }
    SRWLOCK &lock_;
};

NTSTATUS start(SENSOROBJECT sensor) {
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    state.started = state.first = true;
    state.last_timestamp = 0;
    return STATUS_SUCCESS;
}
NTSTATUS stop(SENSOROBJECT sensor) {
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    state.started = false;
    return STATUS_SUCCESS;
}
NTSTATUS fields(SENSOROBJECT sensor, PSENSOR_PROPERTY_LIST output, PULONG size) {
    if (!size) return STATUS_INVALID_PARAMETER;
    *size = SENSOR_PROPERTY_LIST_SIZE(4);
    if (!output) return STATUS_SUCCESS;
    if (output->AllocatedSizeInBytes < *size) return STATUS_BUFFER_TOO_SMALL;
    SENSOR_PROPERTY_LIST_INIT(output, *size);
    output->Count = 4;
    output->List[0] = PKEY_SensorData_Timestamp;
    const auto &keys = sensor_context(sensor)->state->keys();
    std::copy(keys.begin(), keys.end(), output->List + 1);
    return STATUS_SUCCESS;
}
NTSTATUS properties(SENSOROBJECT sensor, PSENSOR_COLLECTION_LIST output, PULONG size) {
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    Collection<4> values;
    values[0].Key = PKEY_Sensor_State;
    InitPropVariantFromUInt32(state.state, &values[0].Value);
    values[1].Key = PKEY_Sensor_MinimumDataInterval_Ms;
    InitPropVariantFromUInt32(4, &values[1].Value);
    values[2].Key = PKEY_Sensor_MaximumDataFieldSize_Bytes;
    InitPropVariantFromUInt32(CollectionsListGetMarshalledSize(state.data.get()), &values[2].Value);
    values[3].Key = PKEY_Sensor_Type;
    InitPropVariantFromCLSID(state.type(), &values[3].Value);
    return values.copy(output, size);
}
NTSTATUS field_properties(SENSOROBJECT sensor, const PROPERTYKEY *key,
                          PSENSOR_COLLECTION_LIST output, PULONG size) {
    if (!key) return STATUS_INVALID_PARAMETER;
    const auto &state = *sensor_context(sensor)->state;
    if (std::none_of(state.keys().begin(), state.keys().end(), [&](auto &entry) { return IsEqualPropertyKey(entry, *key); }))
        return STATUS_NOT_SUPPORTED;
    Collection<3> values;
    const float range = state.gyro ? 2000.0f : 8.0f;
    values[0].Key = PKEY_SensorDataField_Resolution;
    InitPropVariantFromFloat(range * 2 / 65536.0f, &values[0].Value);
    values[1].Key = PKEY_SensorDataField_RangeMinimum;
    InitPropVariantFromFloat(-range, &values[1].Value);
    values[2].Key = PKEY_SensorDataField_RangeMaximum;
    InitPropVariantFromFloat(range, &values[2].Value);
    return values.copy(output, size);
}
NTSTATUS get_interval(SENSOROBJECT sensor, PULONG interval) {
    if (!interval) return STATUS_INVALID_PARAMETER;
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    *interval = state.interval;
    return STATUS_SUCCESS;
}
NTSTATUS set_interval(SENSOROBJECT sensor, ULONG interval) {
    if (!interval) return STATUS_INVALID_PARAMETER;
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    state.interval = std::clamp(interval, 4ul, 1000ul);
    state.first = true;
    return STATUS_SUCCESS;
}
NTSTATUS get_thresholds(SENSOROBJECT sensor, PSENSOR_COLLECTION_LIST output, PULONG size) {
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    Collection<3> values;
    for (unsigned i = 0; i < 3; ++i) {
        values[i].Key = state.keys()[i];
        InitPropVariantFromFloat(state.thresholds[i], &values[i].Value);
    }
    return values.copy(output, size);
}
NTSTATUS set_thresholds(SENSOROBJECT sensor, PSENSOR_COLLECTION_LIST values) {
    if (!values || values->Count > 3 || values->AllocatedSizeInBytes < SENSOR_COLLECTION_LIST_SIZE(values->Count))
        return STATUS_INVALID_PARAMETER;
    auto &state = *sensor_context(sensor)->state;
    Lock guard(state.lock);
    auto next = state.thresholds;
    for (ULONG i = 0; i < values->Count; ++i) {
        const auto &entry = values->List[i];
        const auto &keys = state.keys();
        const auto found = std::find_if(keys.begin(), keys.end(), [&](auto &key) { return IsEqualPropertyKey(key, entry.Key); });
        if (found == keys.end() || entry.Value.vt != VT_R4 || !std::isfinite(entry.Value.fltVal) || entry.Value.fltVal < 0)
            return STATUS_INVALID_PARAMETER;
        next[size_t(found - keys.begin())] = entry.Value.fltVal;
    }
    state.thresholds = next;
    state.first = true;
    return STATUS_SUCCESS;
}
NTSTATUS io_control(SENSOROBJECT, WDFREQUEST, size_t, size_t, ULONG) { return STATUS_NOT_SUPPORTED; }

void publish(DeviceContext *device, const switch2::MotionSnapshot &snapshot) {
    for (auto sensor : device->sensors) {
        auto &state = *sensor_context(sensor)->state;
        Lock guard(state.lock);
        const auto next_state = snapshot.ready ? SensorState_Active : SensorState_Idle;
        if (state.state != next_state) {
            state.state = next_state;
            SensorsCxStateChange(sensor, next_state);
            state.first = true;
        }
        if (!state.started || !snapshot.ready || !snapshot.sample_time_100ns) continue;
        auto axes = state.gyro ? snapshot.angular_velocity : snapshot.acceleration;
        if (!state.gyro) for (auto &value : axes) value /= 9.80665f;
        bool changed = state.first;
        for (unsigned i = 0; i < 3; ++i)
            changed |= std::abs(axes[i] - state.last_axes[i]) >= state.thresholds[i];
        const auto timestamp = snapshot.sample_time_100ns;
        if (timestamp <= state.last_timestamp || !changed) continue;
        // At minimum interval, deliver each hardware sample. Gate slower clients
        // by sample time, never by the arrival time of a multi-sample packet.
        if (!state.first && state.interval > 4 && timestamp - state.last_timestamp < uint64_t(state.interval) * 10000)
            continue;
        const FILETIME time{DWORD(timestamp), DWORD(timestamp >> 32)};
        InitPropVariantFromFileTime(&time, &state.data[0].Value);
        for (unsigned i = 0; i < 3; ++i) InitPropVariantFromFloat(axes[i], &state.data[i + 1].Value);
        SensorsCxSensorDataReady(sensor, state.data.get());
        state.last_timestamp = timestamp;
        state.last_axes = axes;
        state.first = false;
    }
}
void cleanup_sensor(WDFOBJECT sensor) { delete sensor_context(sensor)->state; }
NTSTATUS prepare(WDFDEVICE device, WDFCMRESLIST, WDFCMRESLIST) {
    auto context = device_context(device);
    for (unsigned i = 0; i < 2; ++i) {
        WDF_OBJECT_ATTRIBUTES attributes;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SensorContext);
        attributes.EvtCleanupCallback = cleanup_sensor;
        auto result = SensorsCxSensorCreate(device, &attributes, &context->sensors[i]);
        if (!NT_SUCCESS(result)) return result;
        auto state = new (std::nothrow) SensorState;
        if (!state) return STATUS_INSUFFICIENT_RESOURCES;
        sensor_context(context->sensors[i])->state = state;
        state->gyro = i == 0;
        auto &enumeration = state->enumeration;
        enumeration[0].Key = DEVPKEY_Sensor_Type;
        InitPropVariantFromCLSID(state->type(), &enumeration[0].Value);
        enumeration[1].Key = DEVPKEY_Sensor_Manufacturer;
        InitPropVariantFromString(L"Switch2Native", &enumeration[1].Value);
        enumeration[2].Key = DEVPKEY_Sensor_Model;
        const auto controller = context->slot ? L"Switch 2 Joy-Con " + std::to_wstring(context->slot)
                                             : std::wstring(L"Switch 2 Pro Controller");
        const auto model = controller + (state->gyro ? L" Gyroscope" : L" Accelerometer");
        InitPropVariantFromString(model.c_str(), &enumeration[2].Value);
        enumeration[3].Key = DEVPKEY_Sensor_ConnectionType;
        InitPropVariantFromUInt32(SensorConnectionType_External, &enumeration[3].Value);
        enumeration[4].Key = DEVPKEY_Sensor_PersistentUniqueId;
        GUID identity{0x4e22f400u + context->slot * 2 + i, 0x4aa5, 0x442e, {0xb6,0xad,0xac,0x41,0x7c,0xe8,0x2a,0x71}};
        InitPropVariantFromCLSID(identity, &enumeration[4].Value);
        enumeration[5].Key = DEVPKEY_Sensor_IsPrimary;
        InitPropVariantFromBoolean(FALSE, &enumeration[5].Value);
        state->data[0].Key = PKEY_SensorData_Timestamp;
        const FILETIME zero{};
        InitPropVariantFromFileTime(&zero, &state->data[0].Value);
        for (unsigned axis = 0; axis < 3; ++axis) {
            state->data[axis + 1].Key = state->keys()[axis];
            InitPropVariantFromFloat(0, &state->data[axis + 1].Value);
        }
        SENSOR_CONFIG config;
        SENSOR_CONFIG_INIT(&config);
        config.pEnumerationList = enumeration.get();
        result = SensorsCxSensorInitialize(context->sensors[i], &config);
        if (!NT_SUCCESS(result)) return result;
    }
    return STATUS_SUCCESS;
}
NTSTATUS release(WDFDEVICE device, WDFCMRESLIST) {
    for (auto &sensor : device_context(device)->sensors) {
        if (sensor) WdfObjectDelete(sensor);
        sensor = nullptr;
    }
    return STATUS_SUCCESS;
}
NTSTATUS power_up(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto context = device_context(device);
    try {
        context->reader = new MotionReader([context](auto const &snapshot) { publish(context, snapshot); },
            context->slot ? L"Switch2Native-JoyPad-0" + std::to_wstring(context->slot) : L"Switch2Native-01");
        return STATUS_SUCCESS;
    } catch (...) { return STATUS_INSUFFICIENT_RESOURCES; }
}
NTSTATUS power_down(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto context = device_context(device);
    delete context->reader;
    context->reader = nullptr;
    return STATUS_SUCCESS;
}
void cleanup_device(WDFOBJECT device) { delete device_context(device)->reader; }
NTSTATUS device_add(WDFDRIVER, PWDFDEVICE_INIT init) {
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContext);
    attributes.EvtCleanupCallback = cleanup_device;
    auto result = SensorsCxDeviceInitConfig(init, &attributes, 0);
    if (!NT_SUCCESS(result)) return result;
    WDF_PNPPOWER_EVENT_CALLBACKS power;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
    power.EvtDevicePrepareHardware = prepare;
    power.EvtDeviceReleaseHardware = release;
    power.EvtDeviceD0Entry = power_up;
    power.EvtDeviceD0Exit = power_down;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &power);
    WDFDEVICE device;
    result = WdfDeviceCreate(&init, &attributes, &device);
    if (!NT_SUCCESS(result)) return result;
    WCHAR hardware[128]{};
    ULONG required;
    result = WdfDeviceQueryProperty(device, DevicePropertyHardwareID, sizeof(hardware), hardware, &required);
    if (!NT_SUCCESS(result)) return result;
    for (unsigned slot = 1; slot <= 4; ++slot)
        if (_wcsicmp(hardware, (L"ROOT\\SWITCH2JOYSENSOR" + std::to_wstring(slot)).c_str()) == 0)
            device_context(device)->slot = slot;
    if (!device_context(device)->slot && _wcsicmp(hardware, L"ROOT\\SWITCH2PROSENSOR") != 0)
        return STATUS_INVALID_PARAMETER;
    SENSOR_CONTROLLER_CONFIG config;
    SENSOR_CONTROLLER_CONFIG_INIT(&config);
    config.DriverIsPowerPolicyOwner = WdfUseDefault;
    config.EvtSensorStart = start;
    config.EvtSensorStop = stop;
    config.EvtSensorGetSupportedDataFields = fields;
    config.EvtSensorGetProperties = properties;
    config.EvtSensorGetDataFieldProperties = field_properties;
    config.EvtSensorGetDataInterval = get_interval;
    config.EvtSensorSetDataInterval = set_interval;
    config.EvtSensorGetDataThresholds = get_thresholds;
    config.EvtSensorSetDataThresholds = set_thresholds;
    config.EvtSensorDeviceIoControl = io_control;
    return SensorsCxDeviceInitialize(device, &config);
}
} // namespace
extern "C" DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT object, PUNICODE_STRING registry) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, device_add);
    return WdfDriverCreate(object, registry, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
