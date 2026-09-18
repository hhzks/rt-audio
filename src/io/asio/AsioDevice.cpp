#include "io/asio/AsioLogic.h"

#include <windows.h>
#include <objbase.h>

#include "asiosys.h"
#include "asio.h"
#include "iasiodrv.h"

namespace rt {

static_assert(static_cast<long>(AsioSampleType::Int16MSB) == ASIOSTInt16MSB);
static_assert(static_cast<long>(AsioSampleType::Int32MSB) == ASIOSTInt32MSB);
static_assert(static_cast<long>(AsioSampleType::Float32MSB) == ASIOSTFloat32MSB);
static_assert(static_cast<long>(AsioSampleType::Int16LSB) == ASIOSTInt16LSB);
static_assert(static_cast<long>(AsioSampleType::Int24LSB) == ASIOSTInt24LSB);
static_assert(static_cast<long>(AsioSampleType::Int32LSB) == ASIOSTInt32LSB);
static_assert(static_cast<long>(AsioSampleType::Float32LSB) == ASIOSTFloat32LSB);
static_assert(static_cast<long>(AsioSampleType::Float64LSB) == ASIOSTFloat64LSB);
static_assert(static_cast<long>(AsioSampleType::Int32LSB16) == ASIOSTInt32LSB16);
static_assert(static_cast<long>(AsioSampleType::Int32LSB18) == ASIOSTInt32LSB18);
static_assert(static_cast<long>(AsioSampleType::Int32LSB20) == ASIOSTInt32LSB20);
static_assert(static_cast<long>(AsioSampleType::Int32LSB24) == ASIOSTInt32LSB24);
static_assert(static_cast<long>(AsioSampleType::DSDInt8LSB1) == ASIOSTDSDInt8LSB1);

static_assert(static_cast<long>(AsioSelector::SelectorSupported) == kAsioSelectorSupported);
static_assert(static_cast<long>(AsioSelector::EngineVersion) == kAsioEngineVersion);
static_assert(static_cast<long>(AsioSelector::ResetRequest) == kAsioResetRequest);
static_assert(static_cast<long>(AsioSelector::BufferSizeChange) == kAsioBufferSizeChange);
static_assert(static_cast<long>(AsioSelector::ResyncRequest) == kAsioResyncRequest);
static_assert(static_cast<long>(AsioSelector::LatenciesChanged) == kAsioLatenciesChanged);
static_assert(static_cast<long>(AsioSelector::SupportsTimeInfo) == kAsioSupportsTimeInfo);
static_assert(static_cast<long>(AsioSelector::Overload) == kAsioOverload);

} // namespace rt
