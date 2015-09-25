#include "davis_common.h"

static libusb_device_handle *davisDeviceOpen(libusb_context *devContext, uint16_t devVID, uint16_t devPID,
	uint8_t devType, uint8_t busNumber, uint8_t devAddress);
static void davisDeviceClose(libusb_device_handle *devHandle);
static void davisAllocateTransfers(davisHandle handle, uint32_t bufferNum, uint32_t bufferSize);
static void davisDeallocateTransfers(davisHandle handle);
static void LIBUSB_CALL davisLibUsbCallback(struct libusb_transfer *transfer);
static void davisEventTranslator(davisHandle handle, uint8_t *buffer, size_t bytesSent);
static void *davisDataAcquisitionThread(void *inPtr);
static void davisDataAcquisitionThreadConfig(davisHandle handle);

static inline void checkStrictMonotonicTimestamp(davisHandle handle) {
	if (handle->state.currentTimestamp <= handle->state.lastTimestamp) {
		caerLog(CAER_LOG_ALERT, handle->info.deviceString,
			"Timestamps: non strictly-monotonic timestamp detected: lastTimestamp=%" PRIi32 ", currentTimestamp=%" PRIi32 ", difference=%" PRIi32 ".",
			handle->state.lastTimestamp, handle->state.currentTimestamp,
			(handle->state.lastTimestamp - handle->state.currentTimestamp));
	}
}

static inline void initFrame(davisHandle handle, caerFrameEvent currentFrameEvent) {
	handle->state.apsCurrentReadoutType = APS_READOUT_RESET;
	for (size_t j = 0; j < APS_READOUT_TYPES_NUM; j++) {
		handle->state.apsCountX[j] = 0;
		handle->state.apsCountY[j] = 0;
	}

	if (currentFrameEvent != NULL) {
		// Write out start of frame timestamp.
		caerFrameEventSetTSStartOfFrame(currentFrameEvent, handle->state.currentTimestamp);

		// Setup frame.
		caerFrameEventAllocatePixels(currentFrameEvent, handle->state.apsWindow0SizeX, handle->state.apsWindow0SizeY,
			handle->state.apsChannels);
	}
}

static inline float calculateIMUAccelScale(uint8_t imuAccelScale) {
	// Accelerometer scale is:
	// 0 - +-2 g - 16384 LSB/g
	// 1 - +-4 g - 8192 LSB/g
	// 2 - +-8 g - 4096 LSB/g
	// 3 - +-16 g - 2048 LSB/g
	float accelScale = 65536.0f / (float) U32T(4 * (1 << imuAccelScale));

	return (accelScale);
}

static inline float calculateIMUGyroScale(uint8_t imuGyroScale) {
	// Gyroscope scale is:
	// 0 - +-250 °/s - 131 LSB/°/s
	// 1 - +-500 °/s - 65.5 LSB/°/s
	// 2 - +-1000 °/s - 32.8 LSB/°/s
	// 3 - +-2000 °/s - 16.4 LSB/°/s
	float gyroScale = 65536.0f / (float) U32T(500 * (1 << imuGyroScale));

	return (gyroScale);
}

static inline void freeAllDataMemory(davisState state) {
	if (state->dataExchangeBuffer != NULL) {
		ringBufferFree(state->dataExchangeBuffer);
		state->dataExchangeBuffer = NULL;
	}

	// Since the current event packets aren't necessarily
	// already assigned to the current packet container, we
	// free them separately from it.
	if (state->currentPolarityPacket != NULL) {
		caerEventPacketFree(&state->currentPolarityPacket->packetHeader);
		state->currentPolarityPacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, POLARITY_EVENT, NULL);
		}
	}

	if (state->currentSpecialPacket != NULL) {
		caerEventPacketFree(&state->currentSpecialPacket->packetHeader);
		state->currentSpecialPacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, SPECIAL_EVENT, NULL);
		}
	}

	if (state->currentFramePacket != NULL) {
		caerEventPacketFree(&state->currentFramePacket->packetHeader);
		state->currentFramePacket = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, FRAME_EVENT, NULL);
		}
	}

	if (state->currentIMU6Packet != NULL) {
		caerEventPacketFree(&state->currentIMU6Packet->packetHeader);
		state->currentIMU6Packet = NULL;

		if (state->currentPacketContainer != NULL) {
			caerEventPacketContainerSetEventPacket(state->currentPacketContainer, IMU6_EVENT, NULL);
		}
	}

	if (state->currentPacketContainer != NULL) {
		caerEventPacketContainerFree(state->currentPacketContainer);
		state->currentPacketContainer = NULL;
	}

	if (state->apsCurrentResetFrame != NULL) {
		free(state->apsCurrentResetFrame);
		state->apsCurrentResetFrame = NULL;
	}
}

bool davisCommonClose(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	// Finally, close the device fully.
	davisDeviceClose(state->deviceHandle);

	// Destroy libusb context.
	libusb_exit(state->deviceContext);

	caerLog(CAER_LOG_DEBUG, handle->info.deviceString, "Shutdown successful.");

	// Free memory.
	free(handle->info.deviceString);
	free(handle);

	return (true);
}

caerDavisInfo caerDavisInfoGet(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;

	// Return a link to the device information.
	return (&handle->info);
}

bool davisCommonDataStart(caerDeviceHandle cdh, void (*dataNotifyIncrease)(void *ptr),
	void (*dataNotifyDecrease)(void *ptr), void *dataNotifyUserPtr) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	// Store new data available/not available anymore call-backs.
	state->dataNotifyIncrease = dataNotifyIncrease;
	state->dataNotifyDecrease = dataNotifyDecrease;
	state->dataNotifyUserPtr = dataNotifyUserPtr;

	// Initialize RingBuffer.
	state->dataExchangeBuffer = ringBufferInit(atomic_load(&state->dataExchangeBufferSize));
	if (state->dataExchangeBuffer == NULL) {
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to initialize data exchange buffer.");
		return (false);
	}

	// Allocate packets.
	state->currentPacketContainer = caerEventPacketContainerAllocate(EVENT_TYPES);
	if (state->currentPacketContainer == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate event packet container.");
		return (false);
	}

	state->currentPolarityPacket = caerPolarityEventPacketAllocate(atomic_load(&state->maxPolarityPacketSize),
		(int16_t) handle->info.deviceID, 0);
	if (state->currentPolarityPacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate polarity event packet.");
		return (false);
	}

	state->currentSpecialPacket = caerSpecialEventPacketAllocate(atomic_load(&state->maxSpecialPacketSize),
		(int16_t) handle->info.deviceID, 0);
	if (state->currentSpecialPacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate special event packet.");
		return (false);
	}

	state->currentFramePacket = caerFrameEventPacketAllocate(atomic_load(&state->maxFramePacketSize),
		(int16_t) handle->info.deviceID, 0);
	if (state->currentFramePacket == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate frame event packet.");
		return (false);
	}

	state->currentIMU6Packet = caerIMU6EventPacketAllocate(atomic_load(&state->maxIMU6PacketSize),
		(int16_t) handle->info.deviceID, 0);
	if (state->currentIMU6Packet == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	state->apsCurrentResetFrame = calloc((size_t) state->apsSizeX * state->apsSizeY * state->apsChannels,
		sizeof(uint16_t));
	if (state->currentIMU6Packet == NULL) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	// Default IMU settings (for event parsing).
	state->imuAccelScale = calculateIMUAccelScale(
		U8T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_ACCEL_FULL_SCALE)));
	state->imuGyroScale = calculateIMUGyroScale(
		U8T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_IMU, DAVIS_CONFIG_IMU_GYRO_FULL_SCALE)));

	// Default APS settings (for event parsing).
	state->apsWindow0SizeX =
		U16T(
			spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_COLUMN_0) + 1 - spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_COLUMN_0));
	state->apsWindow0SizeY =
		U16T(
			spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_END_ROW_0) + 1 - spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_START_ROW_0));
	state->apsGlobalShutter = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_GLOBAL_SHUTTER);
	state->apsResetRead = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_RESET_READ);

	// Start data acquisition thread.
	atomic_store(&state->dataAcquisitionThreadRun, true);

	if ((errno = pthread_create(&state->dataAcquisitionThread, NULL, &davisDataAcquisitionThread, handle)) != 0) {
		freeAllDataMemory(state);

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to start data acquisition thread. Error: %d.",
		errno);
		return (false);
	}

	return (true);
}

bool davisCommonDataStop(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;

	// Stop data acquisition thread.
	atomic_store(&state->dataAcquisitionThreadRun, false);

	// Wait for data acquisition thread to terminate...
	if ((errno = pthread_join(state->dataAcquisitionThread, NULL)) != 0) {
		// This should never happen!
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Failed to join data acquisition thread. Error: %d.",
		errno);
		return (false);
	}

	// Empty ringbuffer.
	caerEventPacketContainer container;
	while ((container = ringBufferGet(state->dataExchangeBuffer)) != NULL) {
		// Notify data-not-available call-back.
		state->dataNotifyDecrease(state->dataNotifyUserPtr);

		// Free container, which will free its subordinate packets too.
		caerEventPacketContainerFree(container);
	}

	// Free current, uncommitted packets and ringbuffer.
	freeAllDataMemory(state);

	// Reset packet positions.
	state->currentPolarityPacketPosition = 0;
	state->currentSpecialPacketPosition = 0;
	state->currentFramePacketPosition = 0;
	state->currentIMU6PacketPosition = 0;

	return (true);
}

caerEventPacketContainer davisCommonDataGet(caerDeviceHandle cdh) {
	davisHandle handle = (davisHandle) cdh;
	davisState state = &handle->state;
	caerEventPacketContainer container = NULL;

	retry: container = ringBufferGet(state->dataExchangeBuffer);

	if (container != NULL) {
		// Found an event container, return it and signal this piece of data
		// is no longer available for later acquisition.
		state->dataNotifyDecrease(state->dataNotifyUserPtr);

		return (container);
	}

	// Didn't find any event container, either report this or retry, depending
	// on blocking setting.
	if (atomic_load(&state->dataExchangeBlocking)) {
		goto retry;
	}

	// Nothing.
	return (NULL);
}

void spiConfigSend(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr, uint32_t param) {
	uint8_t spiConfig[4] = { 0 };

	spiConfig[0] = U8T(param >> 24);
	spiConfig[1] = U8T(param >> 16);
	spiConfig[2] = U8T(param >> 8);
	spiConfig[3] = U8T(param >> 0);

	libusb_control_transfer(devHandle, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
	VENDOR_REQUEST_FPGA_CONFIG, moduleAddr, paramAddr, spiConfig, sizeof(spiConfig), 0);
}

uint32_t spiConfigReceive(libusb_device_handle *devHandle, uint8_t moduleAddr, uint8_t paramAddr) {
	uint32_t returnedParam = 0;
	uint8_t spiConfig[4] = { 0 };

	libusb_control_transfer(devHandle, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE,
	VENDOR_REQUEST_FPGA_CONFIG, moduleAddr, paramAddr, spiConfig, sizeof(spiConfig), 0);

	returnedParam |= U32T(spiConfig[0] << 24);
	returnedParam |= U32T(spiConfig[1] << 16);
	returnedParam |= U32T(spiConfig[2] << 8);
	returnedParam |= U32T(spiConfig[3] << 0);

	return (returnedParam);
}

bool davisOpen(davisHandle handle, uint16_t VID, uint16_t PID, uint8_t DID_TYPE, const char *DEVICE_NAME,
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict,
	uint16_t requiredLogicRevision) {
	davisState state = &handle->state;

	// Initialize state variables to default values (if not zero, taken care of by calloc above).
	atomic_store(&state->dataExchangeBufferSize, 64);
	atomic_store(&state->dataExchangeBlocking, false);
	atomic_store(&state->usbBufferNumber, 8);
	atomic_store(&state->usbBufferSize, 4096);

	// Packet settings (size (in events) and time interval (in µs)).
	atomic_store(&state->maxPacketContainerSize, 4096 + 128);
	atomic_store(&state->maxPacketContainerInterval, 5000);
	atomic_store(&state->maxPolarityPacketSize, 4096);
	atomic_store(&state->maxPolarityPacketInterval, 5000);
	atomic_store(&state->maxSpecialPacketSize, 128);
	atomic_store(&state->maxSpecialPacketInterval, 1000);
	atomic_store(&state->maxFramePacketSize, 4);
	atomic_store(&state->maxFramePacketInterval, 50000);
	atomic_store(&state->maxIMU6PacketSize, 8);
	atomic_store(&state->maxIMU6PacketInterval, 5000);

	// Search for device and open it.
	// Initialize libusb using a separate context for each device.
	// This is to correctly support one thread per device.
	if ((errno = libusb_init(&state->deviceContext)) != LIBUSB_SUCCESS) {
		free(handle);

		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to initialize libusb context. Error: %d.", errno);
		return (false);
	}

	// Try to open a DVS128 device on a specific USB port.
	state->deviceHandle = davisDeviceOpen(state->deviceContext, VID, PID, DID_TYPE, busNumberRestrict,
		devAddressRestrict);
	if (state->deviceHandle == NULL) {
		libusb_exit(state->deviceContext);
		free(handle);

		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to open %s device.", DEVICE_NAME);
		return (false);
	}

	// At this point we can get some more precise data on the device and update
	// the logging string to reflect that and be more informative.
	uint8_t busNumber = libusb_get_bus_number(libusb_get_device(state->deviceHandle));
	uint8_t devAddress = libusb_get_device_address(libusb_get_device(state->deviceHandle));

	char serialNumber[8 + 1];
	libusb_get_string_descriptor_ascii(state->deviceHandle, 3, (unsigned char *) serialNumber, 8 + 1);
	serialNumber[8] = '\0'; // Ensure NUL termination.

	size_t fullLogStringLength = (size_t) snprintf(NULL, 0, "%s ID-%" PRIu16 " SN-%s [%" PRIu8 ":%" PRIu8 "]",
		DEVICE_NAME, deviceID, serialNumber, busNumber, devAddress);

	char *fullLogString = malloc(fullLogStringLength + 1);
	if (fullLogString == NULL) {
		libusb_close(state->deviceHandle);
		libusb_exit(state->deviceContext);
		free(handle);

		caerLog(CAER_LOG_CRITICAL, __func__, "Unable to allocate memory for device info string.");
		return (false);
	}

	snprintf(fullLogString, fullLogStringLength + 1, "%s ID-%" PRIu16 " SN-%s [%" PRIu8 ":%" PRIu8 "]", DEVICE_NAME,
		deviceID, serialNumber, busNumber, devAddress);

	// Now check if the Serial Number matches.
	if (!caerStrEquals(serialNumberRestrict, "") && !caerStrEquals(serialNumberRestrict, serialNumber)) {
		libusb_close(state->deviceHandle);
		libusb_exit(state->deviceContext);
		free(handle);

		caerLog(CAER_LOG_CRITICAL, fullLogString, "Device Serial Number doesn't match.");
		free(fullLogString);

		return (false);
	}

	// Populate info variables based on data from device.
	handle->info.deviceID = deviceID;
	handle->info.deviceString = fullLogString;
	handle->info.logicVersion = U16T(
		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_VERSION));
	handle->info.deviceIsMaster = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO,
	DAVIS_CONFIG_SYSINFO_DEVICE_IS_MASTER);
	handle->info.logicClock = U16T(
		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_LOGIC_CLOCK));
	handle->info.adcClock = U16T(
		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_ADC_CLOCK));
	handle->info.chipID = U16T(
		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_SYSINFO, DAVIS_CONFIG_SYSINFO_CHIP_IDENTIFIER));

	handle->info.dvsHasPixelFilter = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS,
	DAVIS_CONFIG_DVS_HAS_PIXEL_FILTER);
	handle->info.dvsHasBackgroundActivityFilter = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS,
	DAVIS_CONFIG_DVS_HAS_BACKGROUND_ACTIVITY_FILTER);
	handle->info.dvsHasTestEventGenerator = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS,
	DAVIS_CONFIG_DVS_HAS_TEST_EVENT_GENERATOR);

	handle->info.apsColorFilter = U8T(
		spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_COLOR_FILTER));
	handle->info.apsHasGlobalShutter = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS,
	DAVIS_CONFIG_APS_HAS_GLOBAL_SHUTTER);
	handle->info.apsHasQuadROI = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_HAS_QUAD_ROI);
	handle->info.apsHasExternalADC = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS,
	DAVIS_CONFIG_APS_HAS_EXTERNAL_ADC);
	handle->info.apsHasInternalADC = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS,
	DAVIS_CONFIG_APS_HAS_INTERNAL_ADC);

	handle->info.extInputHasGenerator = spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_EXTINPUT,
	DAVIS_CONFIG_EXTINPUT_HAS_GENERATOR);

	state->dvsSizeX = U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_COLUMNS));
	state->dvsSizeY = U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_SIZE_ROWS));

	state->dvsInvertXY =
	U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_DVS, DAVIS_CONFIG_DVS_ORIENTATION_INFO)) & 0x04;

	if (state->dvsInvertXY) {
		handle->info.dvsSizeX = state->dvsSizeY;
		handle->info.dvsSizeY = state->dvsSizeX;
	}
	else {
		handle->info.dvsSizeX = state->dvsSizeX;
		handle->info.dvsSizeY = state->dvsSizeY;
	}

	state->apsChannels = (handle->info.apsColorFilter == 0) ? (1) : (4); // RGBG or RGBW are both four channels.

	state->apsSizeX = U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_COLUMNS));
	state->apsSizeY = U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS, DAVIS_CONFIG_APS_SIZE_ROWS));

	uint16_t apsOrientationInfo = U16T(spiConfigReceive(state->deviceHandle, DAVIS_CONFIG_APS_ORIENTATION_INFO, 2));
	state->apsInvertXY = apsOrientationInfo & 0x04;
	state->apsFlipX = apsOrientationInfo & 0x02;
	state->apsFlipY = apsOrientationInfo & 0x01;

	if (state->apsInvertXY) {
		handle->info.apsSizeX = state->apsSizeY;
		handle->info.apsSizeY = state->apsSizeX;
	}
	else {
		handle->info.apsSizeX = state->apsSizeX;
		handle->info.apsSizeY = state->apsSizeY;
	}

	// Verify device logic version.
	if (handle->info.logicVersion < requiredLogicRevision) {
		libusb_close(state->deviceHandle);
		libusb_exit(state->deviceContext);
		free(handle);

		// Logic too old, notify and quit.
		caerLog(CAER_LOG_CRITICAL, fullLogString,
			"Device logic revision too old. You have revision %u; but at least revision %u is required. Please updated by following the Flashy upgrade documentation at 'https://goo.gl/TGM0w1'.",
			handle->info.logicVersion, requiredLogicRevision);
		free(fullLogString);

		return (false);
	}

	caerLog(CAER_LOG_DEBUG, fullLogString, "Initialized device successfully with USB Bus=%" PRIu8 ":Addr=%" PRIu8 ".",
		busNumber, devAddress);

	return (true);
}

bool davisInfoInitialize(davisHandle handle) {

}

bool davisStateInitialize(davisHandle handle) {

}

static libusb_device_handle *davisDeviceOpen(libusb_context *devContext, uint16_t devVID, uint16_t devPID,
	uint8_t devType, uint8_t busNumber, uint8_t devAddress) {
	libusb_device_handle *devHandle = NULL;
	libusb_device **devicesList;

	ssize_t result = libusb_get_device_list(devContext, &devicesList);

	if (result >= 0) {
		// Cycle thorough all discovered devices and find a match.
		for (size_t i = 0; i < (size_t) result; i++) {
			struct libusb_device_descriptor devDesc;

			if (libusb_get_device_descriptor(devicesList[i], &devDesc) != LIBUSB_SUCCESS) {
				continue;
			}

			// Check if this is the device we want (VID/PID).
			if (devDesc.idVendor == devVID && devDesc.idProduct == devPID
				&& (uint8_t) ((devDesc.bcdDevice & 0xFF00) >> 8) == devType) {
				// If a USB port restriction is given, honor it.
				if (busNumber > 0 && libusb_get_bus_number(devicesList[i]) != busNumber) {
					continue;
				}

				if (devAddress > 0 && libusb_get_device_address(devicesList[i]) != devAddress) {
					continue;
				}

				if (libusb_open(devicesList[i], &devHandle) != LIBUSB_SUCCESS) {
					devHandle = NULL;

					continue;
				}

				// Check that the active configuration is set to number 1. If not, do so.
				int activeConfiguration;
				if (libusb_get_configuration(devHandle, &activeConfiguration) != LIBUSB_SUCCESS) {
					libusb_close(devHandle);
					devHandle = NULL;

					continue;
				}

				if (activeConfiguration != 1) {
					if (libusb_set_configuration(devHandle, 1) != LIBUSB_SUCCESS) {
						libusb_close(devHandle);
						devHandle = NULL;

						continue;
					}
				}

				// Claim interface 0 (default).
				if (libusb_claim_interface(devHandle, 0) != LIBUSB_SUCCESS) {
					libusb_close(devHandle);
					devHandle = NULL;

					continue;
				}

				// Found and configured it!
				break;
			}
		}

		libusb_free_device_list(devicesList, true);
	}

	return (devHandle);
}

static void davisDeviceClose(libusb_device_handle *devHandle) {
	// Release interface 0 (default).
	libusb_release_interface(devHandle, 0);

	libusb_close(devHandle);
}

static void davisAllocateTransfers(davisHandle handle, uint32_t bufferNum, uint32_t bufferSize) {
	davisState state = &handle->state;

	// Set number of transfers and allocate memory for the main transfer array.
	state->dataTransfers = calloc(bufferNum, sizeof(struct libusb_transfer *));
	if (state->dataTransfers == NULL) {
		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
			"Failed to allocate memory for %" PRIu32 " libusb transfers. Error: %d.", bufferNum, errno);
		return;
	}
	state->dataTransfersLength = bufferNum;

	// Allocate transfers and set them up.
	for (size_t i = 0; i < bufferNum; i++) {
		state->dataTransfers[i] = libusb_alloc_transfer(0);
		if (state->dataTransfers[i] == NULL) {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to allocate further libusb transfers (%zu of %" PRIu32 ").", i, bufferNum);
			continue;
		}

		// Create data buffer.
		state->dataTransfers[i]->length = (int) bufferSize;
		state->dataTransfers[i]->buffer = malloc(bufferSize);
		if (state->dataTransfers[i]->buffer == NULL) {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to allocate buffer for libusb transfer %zu. Error: %d.", i, errno);

			libusb_free_transfer(state->dataTransfers[i]);
			state->dataTransfers[i] = NULL;

			continue;
		}

		// Initialize Transfer.
		state->dataTransfers[i]->dev_handle = state->deviceHandle;
		state->dataTransfers[i]->endpoint = DATA_ENDPOINT;
		state->dataTransfers[i]->type = LIBUSB_TRANSFER_TYPE_BULK;
		state->dataTransfers[i]->callback = &davisLibUsbCallback;
		state->dataTransfers[i]->user_data = handle;
		state->dataTransfers[i]->timeout = 0;
		state->dataTransfers[i]->flags = LIBUSB_TRANSFER_FREE_BUFFER;

		if ((errno = libusb_submit_transfer(state->dataTransfers[i])) == LIBUSB_SUCCESS) {
			state->activeDataTransfers++;
		}
		else {
			caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
				"Unable to submit libusb transfer %zu. Error: %s (%d).", i, libusb_strerror(errno), errno);

			// The transfer buffer is freed automatically here thanks to
			// the LIBUSB_TRANSFER_FREE_BUFFER flag set above.
			libusb_free_transfer(state->dataTransfers[i]);
			state->dataTransfers[i] = NULL;

			continue;
		}
	}

	if (state->activeDataTransfers == 0) {
		// Didn't manage to allocate any USB transfers, free array memory and log failure.
		free(state->dataTransfers);
		state->dataTransfers = NULL;
		state->dataTransfersLength = 0;

		caerLog(CAER_LOG_CRITICAL, handle->info.deviceString, "Unable to allocate any libusb transfers.");
	}
}

static void davisDeallocateTransfers(davisHandle handle) {
	davisState state = &handle->state;

	// Cancel all current transfers first.
	for (size_t i = 0; i < state->dataTransfersLength; i++) {
		if (state->dataTransfers[i] != NULL) {
			errno = libusb_cancel_transfer(state->dataTransfers[i]);
			if (errno != LIBUSB_SUCCESS && errno != LIBUSB_ERROR_NOT_FOUND) {
				caerLog(CAER_LOG_CRITICAL, handle->info.deviceString,
					"Unable to cancel libusb transfer %zu. Error: %s (%d).", i, libusb_strerror(errno), errno);
				// Proceed with trying to cancel all transfers regardless of errors.
			}
		}
	}

	// Wait for all transfers to go away (0.1 seconds timeout).
	struct timeval te = { .tv_sec = 0, .tv_usec = 100000 };

	while (state->activeDataTransfers > 0) {
		libusb_handle_events_timeout(state->deviceContext, &te);
	}

	// The buffers and transfers have been deallocated in the callback.
	// Only the transfers array remains, which we free here.
	free(state->dataTransfers);
	state->dataTransfers = NULL;
	state->dataTransfersLength = 0;
}

static void LIBUSB_CALL davisLibUsbCallback(struct libusb_transfer *transfer) {
	davisHandle handle = transfer->user_data;
	davisState state = &handle->state;

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		// Handle data.
		davisEventTranslator(handle, transfer->buffer, (size_t) transfer->actual_length);
	}

	if (transfer->status != LIBUSB_TRANSFER_CANCELLED && transfer->status != LIBUSB_TRANSFER_NO_DEVICE) {
		// Submit transfer again.
		if (libusb_submit_transfer(transfer) == LIBUSB_SUCCESS) {
			return;
		}
	}

	// Cannot recover (cancelled, no device, or other critical error).
	// Signal this by adjusting the counter, free and exit.
	state->activeDataTransfers--;
	for (size_t i = 0; i < state->dataTransfersLength; i++) {
		// Remove from list, so we don't try to cancel it later on.
		if (state->dataTransfers[i] == transfer) {
			state->dataTransfers[i] = NULL;
		}
	}
	libusb_free_transfer(transfer);
}

static void davisEventTranslator(davisHandle handle, uint8_t *buffer, size_t bytesSent) {
	// Truncate off any extra partial event.
	if ((bytesSent & 0x01) != 0) {
		caerLog(LOG_ALERT, state->sourceSubSystemString, "%zu bytes received via USB, which is not a multiple of two.",
			bytesSent);
		bytesSent &= (size_t) ~0x01;
	}

	for (size_t i = 0; i < bytesSent; i += 2) {
		bool forcePacketCommit = false;

		uint16_t event = le16toh(*((uint16_t *) (&buffer[i])));

		// Check if timestamp.
		if ((event & 0x8000) != 0) {
			// Is a timestamp! Expand to 32 bits. (Tick is 1µs already.)
			state->lastTimestamp = state->currentTimestamp;
			state->currentTimestamp = state->wrapAdd + (event & 0x7FFF);

			// Check monotonicity of timestamps.
			checkMonotonicTimestamp(state);
		}
		else {
			// Get all current events, so we don't have to duplicate code in every branch.
			caerPolarityEvent currentPolarityEvent = caerPolarityEventPacketGetEvent(state->currentPolarityPacket,
				state->currentPolarityPacketPosition);
			caerFrameEvent currentFrameEvent = caerFrameEventPacketGetEvent(state->currentFramePacket,
				state->currentFramePacketPosition);
			caerIMU6Event currentIMU6Event = caerIMU6EventPacketGetEvent(state->currentIMU6Packet,
				state->currentIMU6PacketPosition);
			caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(state->currentSpecialPacket,
				state->currentSpecialPacketPosition);

			// Look at the code, to determine event and data type.
			uint8_t code = (uint8_t) ((event & 0x7000) >> 12);
			uint16_t data = (event & 0x0FFF);

			switch (code) {
				case 0: // Special event
					switch (data) {
						case 0: // Ignore this, but log it.
							caerLog(LOG_ERROR, state->sourceSubSystemString, "Caught special reserved event!");
							break;

						case 1: { // Timetamp reset
							state->wrapAdd = 0;
							state->lastTimestamp = 0;
							state->currentTimestamp = 0;
							state->dvsTimestamp = 0;

							caerLog(LOG_INFO, state->sourceSubSystemString, "Timestamp reset event received.");

							// Create timestamp reset event.
							caerSpecialEventSetTimestamp(currentSpecialEvent, UINT32_MAX);
							caerSpecialEventSetType(currentSpecialEvent, TIMESTAMP_RESET);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;

							// Commit packets when doing a reset to clearly separate them.
							forcePacketCommit = true;

							// Update Master/Slave status on incoming TS resets.
							//sshsNode sourceInfoNode = caerMainloopGetSourceInfo(state->sourceID);
							//sshsNodePutBool(sourceInfoNode, "deviceIsMaster",
							//	spiConfigReceive(state->deviceHandle, FPGA_SYSINFO, 2));

							break;
						}

						case 2: { // External input (falling edge)
							caerLog(LOG_DEBUG, state->sourceSubSystemString,
								"External input (falling edge) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_FALLING_EDGE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 3: { // External input (rising edge)
							caerLog(LOG_DEBUG, state->sourceSubSystemString,
								"External input (rising edge) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_RISING_EDGE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 4: { // External input (pulse)
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "External input (pulse) event received.");

							caerSpecialEventSetTimestamp(currentSpecialEvent, state->currentTimestamp);
							caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_PULSE);
							caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
							state->currentSpecialPacketPosition++;
							break;
						}

						case 5: { // IMU Start (6 axes)
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "IMU6 Start event received.");

							state->imuIgnoreEvents = false;
							state->imuCount = 0;

							caerIMU6EventSetTimestamp(currentIMU6Event, state->currentTimestamp);
							break;
						}

						case 7: // IMU End
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "IMU End event received.");
							if (state->imuIgnoreEvents) {
								break;
							}

							if (state->imuCount == IMU6_COUNT) {
								caerIMU6EventValidate(currentIMU6Event, state->currentIMU6Packet);
								state->currentIMU6PacketPosition++;
							}
							else {
								caerLog(LOG_INFO, state->sourceSubSystemString,
									"IMU End: failed to validate IMU sample count (%" PRIu8 "), discarding samples.",
									state->imuCount);
							}
							break;

						case 8: { // APS Global Shutter Frame Start
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS GS Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = true;
							state->apsResetRead = true;

							initFrame(state, currentFrameEvent);

							break;
						}

						case 9: { // APS Rolling Shutter Frame Start
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS RS Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = false;
							state->apsResetRead = true;

							initFrame(state, currentFrameEvent);

							break;
						}

						case 10: { // APS Frame End
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Frame End event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							bool validFrame = true;

							for (size_t j = 0; j < APS_READOUT_TYPES_NUM; j++) {
								uint16_t checkValue = caerFrameEventGetLengthX(currentFrameEvent);

								// Check main reset read against zero if disabled.
								if (j == APS_READOUT_RESET && !state->apsResetRead) {
									checkValue = 0;
								}

								caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Frame End: CountX[%zu] is %d.", j,
									state->apsCountX[j]);

								if (state->apsCountX[j] != checkValue) {
									caerLog(LOG_ERROR, state->sourceSubSystemString,
										"APS Frame End: wrong column count [%zu - %d] detected.", j,
										state->apsCountX[j]);
									validFrame = false;
								}
							}

							// Write out end of frame timestamp.
							caerFrameEventSetTSEndOfFrame(currentFrameEvent, state->currentTimestamp);

							// Validate event and advance frame packet position.
							if (validFrame) {
								caerFrameEventValidate(currentFrameEvent, state->currentFramePacket);
							}
							state->currentFramePacketPosition++;

							break;
						}

						case 11: { // APS Reset Column Start
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Reset Column Start event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							state->apsCurrentReadoutType = APS_READOUT_RESET;
							state->apsCountY[state->apsCurrentReadoutType] = 0;

							state->apsRGBPixelOffsetDirection = 0;
							state->apsRGBPixelOffset = 1; // RGB support, first pixel of row always even.

							// The first Reset Column Read Start is also the start
							// of the exposure for the RS.
							if (!state->apsGlobalShutter && state->apsCountX[APS_READOUT_RESET] == 0) {
								caerFrameEventSetTSStartOfExposure(currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 12: { // APS Signal Column Start
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Signal Column Start event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							state->apsCurrentReadoutType = APS_READOUT_SIGNAL;
							state->apsCountY[state->apsCurrentReadoutType] = 0;

							state->apsRGBPixelOffsetDirection = 0;
							state->apsRGBPixelOffset = 1; // RGB support, first pixel of row always even.

							// The first Signal Column Read Start is also always the end
							// of the exposure time, for both RS and GS.
							if (state->apsCountX[APS_READOUT_SIGNAL] == 0) {
								caerFrameEventSetTSEndOfExposure(currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 13: { // APS Column End
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Column End event received.");
							if (state->apsIgnoreEvents) {
								break;
							}

							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Column End: CountX[%d] is %d.",
								state->apsCurrentReadoutType, state->apsCountX[state->apsCurrentReadoutType]);
							caerLog(LOG_DEBUG, state->sourceSubSystemString, "APS Column End: CountY[%d] is %d.",
								state->apsCurrentReadoutType, state->apsCountY[state->apsCurrentReadoutType]);

							if (state->apsCountY[state->apsCurrentReadoutType]
								!= caerFrameEventGetLengthY(currentFrameEvent)) {
								caerLog(LOG_ERROR, state->sourceSubSystemString,
									"APS Column End: wrong row count [%d - %d] detected.", state->apsCurrentReadoutType,
									state->apsCountY[state->apsCurrentReadoutType]);
							}

							state->apsCountX[state->apsCurrentReadoutType]++;

							// The last Reset Column Read End is also the start
							// of the exposure for the GS.
							if (state->apsGlobalShutter && state->apsCurrentReadoutType == APS_READOUT_RESET
								&& state->apsCountX[APS_READOUT_RESET] == caerFrameEventGetLengthX(currentFrameEvent)) {
								caerFrameEventSetTSStartOfExposure(currentFrameEvent, state->currentTimestamp);
							}

							break;
						}

						case 14: { // APS Global Shutter Frame Start with no Reset Read
							caerLog(LOG_DEBUG, state->sourceSubSystemString,
								"APS GS NORST Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = true;
							state->apsResetRead = false;

							initFrame(state, currentFrameEvent);

							// If reset reads are disabled, the start of exposure is closest to
							// the start of frame.
							caerFrameEventSetTSStartOfExposure(currentFrameEvent, state->currentTimestamp);

							break;
						}

						case 15: { // APS Rolling Shutter Frame Start with no Reset Read
							caerLog(LOG_DEBUG, state->sourceSubSystemString,
								"APS RS NORST Frame Start event received.");
							state->apsIgnoreEvents = false;
							state->apsGlobalShutter = false;
							state->apsResetRead = false;

							initFrame(state, currentFrameEvent);

							// If reset reads are disabled, the start of exposure is closest to
							// the start of frame.
							caerFrameEventSetTSStartOfExposure(currentFrameEvent, state->currentTimestamp);

							break;
						}

						case 16:
						case 17:
						case 18:
						case 19:
						case 20:
						case 21:
						case 22:
						case 23:
						case 24:
						case 25:
						case 26:
						case 27:
						case 28:
						case 29:
						case 30:
						case 31: {
							caerLog(LOG_DEBUG, state->sourceSubSystemString,
								"IMU Scale Config event (%" PRIu16 ") received.", data);
							if (state->imuIgnoreEvents) {
								break;
							}

							// Set correct IMU accel and gyro scales, used to interpret subsequent
							// IMU samples from the device.
							state->imuAccelScale = calculateIMUAccelScale((data >> 2) & 0x03);
							state->imuGyroScale = calculateIMUGyroScale(data & 0x03);

							// At this point the IMU event count should be zero (reset by start).
							if (state->imuCount != 0) {
								caerLog(LOG_INFO, state->sourceSubSystemString,
									"IMU Scale Config: previous IMU start event missed, attempting recovery.");
							}

							// Increase IMU count by one, to a total of one (0+1=1).
							// This way we can recover from the above error of missing start, and we can
							// later discover if the IMU Scale Config event actually arrived itself.
							state->imuCount = 1;

							break;
						}

						default:
							caerLog(LOG_ERROR, state->sourceSubSystemString,
								"Caught special event that can't be handled: %d.", data);
							break;
					}
					break;

				case 1: // Y address
					// Check range conformity.
					if (data >= state->dvsSizeY) {
						caerLog(LOG_ALERT, state->sourceSubSystemString,
							"DVS: Y address out of range (0-%d): %" PRIu16 ".", state->dvsSizeY - 1, data);
						break; // Skip invalid Y address (don't update lastY).
					}

					if (state->dvsGotY) {
						// Use the previous timestamp here, since this refers to the previous Y.
						caerSpecialEventSetTimestamp(currentSpecialEvent, state->dvsTimestamp);
						caerSpecialEventSetType(currentSpecialEvent, DVS_ROW_ONLY);
						caerSpecialEventSetData(currentSpecialEvent, state->dvsLastY);
						caerSpecialEventValidate(currentSpecialEvent, state->currentSpecialPacket);
						state->currentSpecialPacketPosition++;

						caerLog(LOG_DEBUG, state->sourceSubSystemString,
							"DVS: row-only event received for address Y=%" PRIu16 ".", state->dvsLastY);
					}

					state->dvsLastY = data;
					state->dvsGotY = true;
					state->dvsTimestamp = state->currentTimestamp;

					break;

				case 2: // X address, Polarity OFF
				case 3: { // X address, Polarity ON
					// Check range conformity.
					if (data >= state->dvsSizeX) {
						caerLog(LOG_ALERT, state->sourceSubSystemString,
							"DVS: X address out of range (0-%d): %" PRIu16 ".", state->dvsSizeX - 1, data);
						break; // Skip invalid event.
					}

					// Invert polarity for PixelParade high gain pixels (DavisSense), because of
					// negative gain from pre-amplifier.
					uint8_t polarity = ((state->chipID == CHIP_DAVIS208) && (data < 192)) ? ((uint8_t) ~code) : (code);

					caerPolarityEventSetTimestamp(currentPolarityEvent, state->dvsTimestamp);
					caerPolarityEventSetPolarity(currentPolarityEvent, (polarity & 0x01));
					if (state->dvsInvertXY) {
						caerPolarityEventSetY(currentPolarityEvent, data);
						caerPolarityEventSetX(currentPolarityEvent, state->dvsLastY);
					}
					else {
						caerPolarityEventSetY(currentPolarityEvent, state->dvsLastY);
						caerPolarityEventSetX(currentPolarityEvent, data);
					}
					caerPolarityEventValidate(currentPolarityEvent, state->currentPolarityPacket);
					state->currentPolarityPacketPosition++;

					state->dvsGotY = false;

					break;
				}

				case 4: {
					if (state->apsIgnoreEvents) {
						break;
					}

					// Let's check that apsCountY is not above the maximum. This could happen
					// if start/end of column events are discarded (no wait on transfer stall).
					if (state->apsCountY[state->apsCurrentReadoutType] >= caerFrameEventGetLengthY(currentFrameEvent)) {
						caerLog(LOG_DEBUG, state->sourceSubSystemString,
							"APS ADC sample: row count is at maximum, discarding further samples.");
						break;
					}

					// If reset read, we store the values in a local array. If signal read, we
					// store the final pixel value directly in the output frame event. We already
					// do the subtraction between reset and signal here, to avoid carrying that
					// around all the time and consuming memory. This way we can also only take
					// infrequent reset reads and re-use them for multiple frames, which can heavily
					// reduce traffic, and should not impact image quality heavily, at least in GS.
					uint16_t xPos =
						(state->apsFlipX) ?
							(U16T(
								caerFrameEventGetLengthX(currentFrameEvent) - 1
									- state->apsCountX[state->apsCurrentReadoutType])) :
							(U16T(state->apsCountX[state->apsCurrentReadoutType]));
					uint16_t yPos =
						(state->apsFlipY) ?
							(U16T(
								caerFrameEventGetLengthY(currentFrameEvent) - 1
									- state->apsCountY[state->apsCurrentReadoutType])) :
							(U16T(state->apsCountY[state->apsCurrentReadoutType]));

					if (state->chipID == CHIP_DAVISRGB) {
						yPos = U16T(yPos + state->apsRGBPixelOffset);
					}

					if (state->apsInvertXY) {
						SWAP_VAR(uint16_t, xPos, yPos);
					}

					size_t pixelPosition = (size_t) (yPos * caerFrameEventGetLengthX(currentFrameEvent)) + xPos;

					uint16_t xPosAbs = U16T(xPos + state->apsWindow0StartX);
					uint16_t yPosAbs = U16T(yPos + state->apsWindow0StartY);
					size_t pixelPositionAbs = (size_t) (yPosAbs * state->apsSizeX) + xPosAbs;

					if ((state->apsCurrentReadoutType == APS_READOUT_RESET
						&& !(state->chipID == CHIP_DAVISRGB && state->apsGlobalShutter))
						|| (state->apsCurrentReadoutType == APS_READOUT_SIGNAL
							&& (state->chipID == CHIP_DAVISRGB && state->apsGlobalShutter))) {
						state->apsCurrentResetFrame[pixelPositionAbs] = data;
					}
					else {
						int32_t pixelValue = 0;

						if (state->chipID == CHIP_DAVISRGB && state->apsGlobalShutter) {
							// DAVIS RGB GS has inverted samples, signal read comes first
							// and was stored above inside state->apsCurrentResetFrame.
							pixelValue = (data - state->apsCurrentResetFrame[pixelPositionAbs]);
						}
						else {
							pixelValue = (state->apsCurrentResetFrame[pixelPositionAbs] - data);
						}

						// Normalize the ADC value to 16bit generic depth and check for underflow.
						pixelValue = (pixelValue < 0) ? (0) : (pixelValue);
						pixelValue = pixelValue << (16 - DAVIS_ADC_DEPTH);

						caerFrameEventGetPixelArrayUnsafe(currentFrameEvent)[pixelPosition] = htole16(U16T(pixelValue));
					}

					caerLog(LOG_DEBUG, state->sourceSubSystemString,
						"APS ADC Sample: column=%" PRIu16 ", row=%" PRIu16 ", xPos=%" PRIu16 ", yPos=%" PRIu16 ", data=%" PRIu16 ".",
						state->apsCountX[state->apsCurrentReadoutType], state->apsCountY[state->apsCurrentReadoutType],
						xPos, yPos, data);

					state->apsCountY[state->apsCurrentReadoutType]++;

					// RGB support: first 320 pixels are even, then odd.
					if (state->chipID == CHIP_DAVISRGB) {
						if (state->apsRGBPixelOffsetDirection == 0) { // Increasing
							state->apsRGBPixelOffset++;

							if (state->apsRGBPixelOffset == 321) {
								// Switch to decreasing after last even pixel.
								state->apsRGBPixelOffsetDirection = 1;
								state->apsRGBPixelOffset = 318;
							}
						}
						else { // Decreasing
							state->apsRGBPixelOffset = (int16_t) (state->apsRGBPixelOffset - 3);
						}
					}

					break;
				}

				case 5: {
					// Misc 8bit data, used currently only
					// for IMU events in DAVIS FX3 boards.
					uint8_t misc8Code = U8T((data & 0x0F00) >> 8);
					uint8_t misc8Data = U8T(data & 0x00FF);

					switch (misc8Code) {
						case 0:
							if (state->imuIgnoreEvents) {
								break;
							}

							// Detect missing IMU end events.
							if (state->imuCount >= IMU6_COUNT) {
								caerLog(LOG_INFO, state->sourceSubSystemString,
									"IMU data: IMU samples count is at maximum, discarding further samples.");
								break;
							}

							// IMU data event.
							switch (state->imuCount) {
								case 0:
									caerLog(LOG_ERROR, state->sourceSubSystemString,
										"IMU data: missing IMU Scale Config event. Parsing of IMU events will still be attempted, but be aware that Accel/Gyro scale conversions may be inaccurate.");
									state->imuCount = 1;
									// Fall through to next case, as if imuCount was equal to 1.

								case 1:
								case 3:
								case 5:
								case 7:
								case 9:
								case 11:
								case 13:
									state->imuTmpData = misc8Data;
									break;

								case 2: {
									int16_t accelX = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelX(currentIMU6Event, accelX / state->imuAccelScale);
									break;
								}

								case 4: {
									int16_t accelY = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelY(currentIMU6Event, accelY / state->imuAccelScale);
									break;
								}

								case 6: {
									int16_t accelZ = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetAccelZ(currentIMU6Event, accelZ / state->imuAccelScale);
									break;
								}

									// Temperature is signed. Formula for converting to °C:
									// (SIGNED_VAL / 340) + 36.53
								case 8: {
									int16_t temp = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetTemp(currentIMU6Event, (temp / 340.0f) + 36.53f);
									break;
								}

								case 10: {
									int16_t gyroX = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroX(currentIMU6Event, gyroX / state->imuGyroScale);
									break;
								}

								case 12: {
									int16_t gyroY = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroY(currentIMU6Event, gyroY / state->imuGyroScale);
									break;
								}

								case 14: {
									int16_t gyroZ = (int16_t) ((state->imuTmpData << 8) | misc8Data);
									caerIMU6EventSetGyroZ(currentIMU6Event, gyroZ / state->imuGyroScale);
									break;
								}
							}

							state->imuCount++;

							break;

						default:
							caerLog(LOG_ERROR, state->sourceSubSystemString,
								"Caught Misc8 event that can't be handled.");
							break;
					}

					break;
				}

				case 7: // Timestamp wrap
					// Each wrap is 2^15 µs (~32ms), and we have
					// to multiply it with the wrap counter,
					// which is located in the data part of this
					// event.
					state->wrapAdd += (uint32_t) (0x8000 * data);

					state->lastTimestamp = state->currentTimestamp;
					state->currentTimestamp = state->wrapAdd;

					// Check monotonicity of timestamps.
					checkMonotonicTimestamp(state);

					caerLog(LOG_DEBUG, state->sourceSubSystemString,
						"Timestamp wrap event received with multiplier of %" PRIu16 ".", data);
					break;

				default:
					caerLog(LOG_ERROR, state->sourceSubSystemString, "Caught event that can't be handled.");
					break;
			}
		}

		// Commit packet to the ring-buffer, so they can be processed by the
		// main-loop, when their stated conditions are met.
		if (forcePacketCommit
			|| (state->currentPolarityPacketPosition
				>= caerEventPacketHeaderGetEventCapacity(&state->currentPolarityPacket->packetHeader))
			|| ((state->currentPolarityPacketPosition > 1)
				&& (caerPolarityEventGetTimestamp(
					caerPolarityEventPacketGetEvent(state->currentPolarityPacket,
						state->currentPolarityPacketPosition - 1))
					- caerPolarityEventGetTimestamp(caerPolarityEventPacketGetEvent(state->currentPolarityPacket, 0))
					>= state->maxPolarityPacketInterval))) {
			if (!ringBufferPut(state->dataExchangeBuffer, state->currentPolarityPacket)) {
				// Failed to forward packet, drop it.
				free(state->currentPolarityPacket);
				caerLog(LOG_INFO, state->sourceSubSystemString,
					"Dropped Polarity Event Packet because ring-buffer full!");
			}
			else {
				caerMainloopDataAvailableIncrease(state->mainloopNotify);
			}

			// Allocate new packet for next iteration.
			state->currentPolarityPacket = caerPolarityEventPacketAllocate(state->maxPolarityPacketSize,
				state->sourceID);
			state->currentPolarityPacketPosition = 0;
		}

		if (forcePacketCommit
			|| (state->currentFramePacketPosition
				>= caerEventPacketHeaderGetEventCapacity(&state->currentFramePacket->packetHeader))
			|| ((state->currentFramePacketPosition > 1)
				&& (caerFrameEventGetTSStartOfExposure(
					caerFrameEventPacketGetEvent(state->currentFramePacket, state->currentFramePacketPosition - 1))
					- caerFrameEventGetTSStartOfExposure(caerFrameEventPacketGetEvent(state->currentFramePacket, 0))
					>= state->maxFramePacketInterval))) {
			if (!ringBufferPut(state->dataExchangeBuffer, state->currentFramePacket)) {
				// Failed to forward packet, drop it.
				free(state->currentFramePacket);
				caerLog(LOG_INFO, state->sourceSubSystemString, "Dropped Frame Event Packet because ring-buffer full!");
			}
			else {
				caerMainloopDataAvailableIncrease(state->mainloopNotify);
			}

			// Allocate new packet for next iteration.
			state->currentFramePacket = caerFrameEventPacketAllocate(state->maxFramePacketSize, state->sourceID,
				state->apsSizeX, state->apsSizeY, DAVIS_COLOR_CHANNELS);
			state->currentFramePacketPosition = 0;

			// Ignore all APS events, until a new APS Start event comes in.
			// This is to correctly support the forced packet commits that a TS reset,
			// or a timeout condition, impose. Continuing to parse events would result
			// in a corrupted state of the first event in the new packet, as it would
			// be incomplete and miss vital initialization data.
			state->apsIgnoreEvents = true;
		}

		if (forcePacketCommit
			|| (state->currentIMU6PacketPosition
				>= caerEventPacketHeaderGetEventCapacity(&state->currentIMU6Packet->packetHeader))
			|| ((state->currentIMU6PacketPosition > 1)
				&& (caerIMU6EventGetTimestamp(
					caerIMU6EventPacketGetEvent(state->currentIMU6Packet, state->currentIMU6PacketPosition - 1))
					- caerIMU6EventGetTimestamp(caerIMU6EventPacketGetEvent(state->currentIMU6Packet, 0))
					>= state->maxIMU6PacketInterval))) {
			if (!ringBufferPut(state->dataExchangeBuffer, state->currentIMU6Packet)) {
				// Failed to forward packet, drop it.
				free(state->currentIMU6Packet);
				caerLog(LOG_INFO, state->sourceSubSystemString, "Dropped IMU6 Event Packet because ring-buffer full!");
			}
			else {
				caerMainloopDataAvailableIncrease(state->mainloopNotify);
			}

			// Allocate new packet for next iteration.
			state->currentIMU6Packet = caerIMU6EventPacketAllocate(state->maxIMU6PacketSize, state->sourceID);
			state->currentIMU6PacketPosition = 0;

			// Ignore all IMU events, until a new IMU Start event comes in.
			// This is to correctly support the forced packet commits that a TS reset,
			// or a timeout condition, impose. Continuing to parse events would result
			// in a corrupted state of the first event in the new packet, as it would
			// be incomplete and miss vital initialization data.
			state->imuIgnoreEvents = true;
		}

		if (forcePacketCommit
			|| (state->currentSpecialPacketPosition
				>= caerEventPacketHeaderGetEventCapacity(&state->currentSpecialPacket->packetHeader))
			|| ((state->currentSpecialPacketPosition > 1)
				&& (caerSpecialEventGetTimestamp(
					caerSpecialEventPacketGetEvent(state->currentSpecialPacket,
						state->currentSpecialPacketPosition - 1))
					- caerSpecialEventGetTimestamp(caerSpecialEventPacketGetEvent(state->currentSpecialPacket, 0))
					>= state->maxSpecialPacketInterval))) {
			retry_special: if (!ringBufferPut(state->dataExchangeBuffer, state->currentSpecialPacket)) {
				// Failed to forward packet, drop it, unless it contains a timestamp
				// related change, those are critical, so we just spin until we can
				// deliver that one. (Easily detected by forcePacketCommit!)
				if (forcePacketCommit) {
					goto retry_special;
				}
				else {
					// Failed to forward packet, drop it.
					free(state->currentSpecialPacket);
					caerLog(LOG_INFO, state->sourceSubSystemString,
						"Dropped Special Event Packet because ring-buffer full!");
				}
			}
			else {
				caerMainloopDataAvailableIncrease(state->mainloopNotify);
			}

			// Allocate new packet for next iteration.
			state->currentSpecialPacket = caerSpecialEventPacketAllocate(state->maxSpecialPacketSize, state->sourceID);
			state->currentSpecialPacketPosition = 0;
		}
	}
}
