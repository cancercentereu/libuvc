/**
 * @defgroup device Device handling and enumeration
 */

#include "libuvc/libuvc.h"
#include "libuvc/libuvc_internal.h"

int uvc_already_open(uvc_context_t *ctx, struct libusb_device *usb_dev);
uvc_error_t uvc_claim_ifs(uvc_device_handle_t *devh);
void uvc_release_ifs(uvc_device_handle_t *devh);
void uvc_free_devh(uvc_device_handle_t *devh);

uvc_error_t uvc_get_device_info(uvc_device_t *dev, uvc_device_info_t **info);
void uvc_free_device_info(uvc_device_info_t *info);

uvc_error_t uvc_scan_control(uvc_device_t *dev, uvc_device_info_t *info);
uvc_error_t uvc_parse_vc(uvc_device_t *dev,
			 uvc_device_info_t *info,
			 const unsigned char *block, size_t block_size);
uvc_error_t uvc_parse_vc_extension_unit(uvc_device_t *dev,
					uvc_device_info_t *info,
					const unsigned char *block,
					size_t block_size);
uvc_error_t uvc_parse_vc_header(uvc_device_t *dev,
				uvc_device_info_t *info,
				const unsigned char *block, size_t block_size);
uvc_error_t uvc_parse_vc_input_terminal(uvc_device_t *dev,
					uvc_device_info_t *info,
					const unsigned char *block,
					size_t block_size);
uvc_error_t uvc_parse_vc_processing_unit(uvc_device_t *dev,
					 uvc_device_info_t *info,
					 const unsigned char *block,
					 size_t block_size);

uvc_error_t uvc_scan_streaming(uvc_device_t *dev,
			       uvc_device_info_t *info,
			       int interface_idx);
uvc_error_t uvc_parse_vs(uvc_device_t *dev,
			 uvc_device_info_t *info,
			 uvc_streaming_interface_t *stream_if,
			 const unsigned char *block, size_t block_size);
uvc_error_t uvc_parse_vs_format_uncompressed(uvc_streaming_interface_t *stream_if,
					     const unsigned char *block,
					     size_t block_size);
uvc_error_t uvc_parse_vs_frame_uncompressed(uvc_streaming_interface_t *stream_if,
					    const unsigned char *block,
					    size_t block_size);
uvc_error_t uvc_parse_vs_input_header(uvc_streaming_interface_t *stream_if,
				      const unsigned char *block,
				      size_t block_size);

void _uvc_status_callback(struct libusb_transfer *transfer);

/** @internal
 * @brief Test whether the specified USB device has been opened as a UVC device
 * @ingroup device
 *
 * @param ctx Context in which to search for the UVC device
 * @param usb_dev USB device to find
 * @return true if the device is open in this context
 */
int uvc_already_open(uvc_context_t *ctx, struct libusb_device *usb_dev) {
  uvc_device_handle_t *devh;

  DL_FOREACH(ctx->open_devices, devh) {
    if (usb_dev == devh->dev->usb_dev)
      return 1;
  }

  return 0;
}

/** @brief Finds a camera identified by vendor, product and/or serial number
 * @ingroup device
 *
 * @param[in] ctx UVC context in which to search for the camera
 * @param[out] dev Reference to the camera, or NULL if not found
 * @param[in] vid Vendor ID number, optional
 * @param[in] pid Product ID number, optional
 * @param[in] sn Serial number or NULL
 * @return Error finding device or UVC_SUCCESS
 */
uvc_error_t uvc_find_device(
    uvc_context_t *ctx, uvc_device_t **dev,
    int vid, int pid, const char *sn) {
  uvc_error_t ret = UVC_SUCCESS;

  uvc_device_t **list;
  uvc_device_t *test_dev;
  int dev_idx;
  int found_dev;

  ret = uvc_get_device_list(ctx, &list);

  if (ret != UVC_SUCCESS)
    return ret;

  dev_idx = 0;
  found_dev = 0;

  while (!found_dev && (test_dev = list[dev_idx++]) != NULL) {
    uvc_device_descriptor_t *desc;

    if (uvc_get_device_descriptor(test_dev, &desc) != UVC_SUCCESS)
      continue;

    if ((!vid || desc->idVendor == vid)
        && (!pid || desc->idProduct == pid)
        && (!sn || (desc->serialNumber && !strcmp(desc->serialNumber, sn))))
      found_dev = 1;

    uvc_free_device_descriptor(desc);
  }

  if (found_dev)
    uvc_ref_device(test_dev);

  uvc_free_device_list(list, 1);

  if (found_dev) {
    *dev = test_dev;
    return UVC_SUCCESS;
  } else {
    return UVC_ERROR_NO_DEVICE;
  }
}

/** @brief Open a UVC device
 * @ingroup device
 *
 * @param dev Device to open
 * @param[out] devh Handle on opened device
 * @return Error opening device or SUCCESS
 */
uvc_error_t uvc_open(
    uvc_device_t *dev,
    uvc_device_handle_t **devh) {
  uvc_error_t ret;
  struct libusb_device_handle *usb_devh;
  uvc_device_handle_t *internal_devh;
  struct libusb_device_descriptor desc;

  ret = libusb_open(dev->usb_dev, &usb_devh);

  if (ret != UVC_SUCCESS)
    return ret;

  uvc_ref_device(dev);

  internal_devh = calloc(1, sizeof(*internal_devh));
  internal_devh->dev = dev;
  internal_devh->usb_devh = usb_devh;

  ret = uvc_get_device_info(dev, &(internal_devh->info));
  if (ret != UVC_SUCCESS)
    goto fail;

  ret = uvc_claim_ifs(internal_devh);
  if (ret != UVC_SUCCESS)
    goto fail;

  libusb_get_device_descriptor(dev->usb_dev, &desc);
  internal_devh->is_isight = (desc.idVendor == 0x05ac && desc.idProduct == 0x8501);

  if (internal_devh->info->ctrl_if.bEndpointAddress) {
    internal_devh->status_xfer = libusb_alloc_transfer(0);
    if (!internal_devh->status_xfer) {
      ret = UVC_ERROR_NO_MEM;
      goto fail;
    }

    libusb_fill_interrupt_transfer(internal_devh->status_xfer,
                                   usb_devh,
                                   internal_devh->info->ctrl_if.bEndpointAddress,
                                   internal_devh->status_buf,
                                   sizeof(internal_devh->status_buf),
                                   _uvc_status_callback,
                                   internal_devh,
                                   0);
    ret = libusb_submit_transfer(internal_devh->status_xfer);

    if (ret) {
      fprintf(stderr,
              "uvc: device has a status interrupt endpoint, but unable to read from it\n");
      goto fail;
    }
  }

  DL_APPEND(dev->ctx->open_devices, internal_devh);
  *devh = internal_devh;

  return ret;

 fail:
  uvc_release_ifs(internal_devh);
  libusb_close(usb_devh);
  uvc_unref_device(dev);
  uvc_free_devh(internal_devh);

  return ret;
}

/**
 * @internal
 * @brief Parses the complete device descriptor for a device
 * @ingroup device
 * @note Free *info with uvc_free_device_info when you're done
 *
 * @param dev Device to parse descriptor for
 * @param info Where to store a pointer to the new info struct
 */
uvc_error_t uvc_get_device_info(uvc_device_t *dev,
				uvc_device_info_t **info) {
  uvc_error_t ret;
  uvc_device_info_t *internal_info;

  internal_info = calloc(1, sizeof(*internal_info));
  if (!internal_info)
    return UVC_ERROR_NO_MEM;

  if (libusb_get_config_descriptor(dev->usb_dev,
				   0,
				   &(internal_info->config)) != 0) {
    free(internal_info);
    return UVC_ERROR_IO;
  }

  ret = uvc_scan_control(dev, internal_info);
  if (ret != UVC_SUCCESS) {
    uvc_free_device_info(internal_info);
    return ret;
  }

  *info = internal_info;

  return ret;
}

/**
 * @internal
 * @brief Frees the device descriptor for a device
 * @ingroup device
 *
 * @param info Which device info block to free
 */
void uvc_free_device_info(uvc_device_info_t *info) {
  uvc_input_terminal_t *input_term, *input_term_tmp;
  uvc_processing_unit_t *proc_unit, *proc_unit_tmp;
  uvc_extension_unit_t *ext_unit, *ext_unit_tmp;

  uvc_streaming_interface_t *stream_if, *stream_if_tmp;
  uvc_format_desc_t *format, *format_tmp;
  uvc_frame_desc_t *frame, *frame_tmp;

  DL_FOREACH_SAFE(info->ctrl_if.input_term_descs, input_term, input_term_tmp) {
    DL_DELETE(info->ctrl_if.input_term_descs, input_term);
    free(input_term);
  }

  DL_FOREACH_SAFE(info->ctrl_if.processing_unit_descs, proc_unit, proc_unit_tmp) {
    DL_DELETE(info->ctrl_if.processing_unit_descs, proc_unit);
    free(proc_unit);
  }

  DL_FOREACH_SAFE(info->ctrl_if.extension_unit_descs, ext_unit, ext_unit_tmp) {
    DL_DELETE(info->ctrl_if.extension_unit_descs, ext_unit);
    free(ext_unit);
  }

  DL_FOREACH_SAFE(info->stream_ifs, stream_if, stream_if_tmp) {
    DL_FOREACH_SAFE(stream_if->format_descs, format, format_tmp) {
      DL_FOREACH_SAFE(format->frame_descs, frame, frame_tmp) {
        if (frame->intervals)
          free(frame->intervals);

        DL_DELETE(format->frame_descs, frame);
        free(frame);
      }

      DL_DELETE(stream_if->format_descs, format);
      free(format);
    }

    DL_DELETE(info->stream_ifs, stream_if);
    free(stream_if);
  }

  if (info->config)
    libusb_free_config_descriptor(info->config);

  free(info);
}

/**
 * @brief Get a descriptor that contains the general information about
 * a device
 * @ingroup device
 *
 * Free *desc with uvc_free_device_descriptor when you're done.
 *
 * @param dev Device to fetch information about
 * @param[out] desc Descriptor structure
 * @return Error if unable to fetch information, else SUCCESS
 */
uvc_error_t uvc_get_device_descriptor(
    uvc_device_t *dev,
    uvc_device_descriptor_t **desc) {
  uvc_device_descriptor_t *desc_internal;
  struct libusb_device_descriptor usb_desc;
  struct libusb_device_handle *usb_devh;
  uvc_error_t ret;

  ret = libusb_get_device_descriptor(dev->usb_dev, &usb_desc);

  if (ret != UVC_SUCCESS)
    return ret;

  desc_internal = calloc(1, sizeof(*desc_internal));
  desc_internal->idVendor = usb_desc.idVendor;
  desc_internal->idProduct = usb_desc.idProduct;

  if (libusb_open(dev->usb_dev, &usb_devh) == 0) {
    unsigned char serial_buf[64];

    int serial_bytes = libusb_get_string_descriptor_ascii(
        usb_devh, usb_desc.iSerialNumber, serial_buf, sizeof(serial_buf));

    if (serial_bytes > 0)
      desc_internal->serialNumber = strdup((const char*) serial_buf);

    /** @todo get manufacturer, product names */

    libusb_close(usb_devh);
  }

  *desc = desc_internal;

  return ret;
}

/**
 * @brief Frees a device descriptor created with uvc_get_device_descriptor
 * @ingroup device
 *
 * @param desc Descriptor to free
 */
void uvc_free_device_descriptor(
    uvc_device_descriptor_t *desc) {
  if (desc->serialNumber)
    free((void*) desc->serialNumber);

  if (desc->manufacturer)
    free((void*) desc->manufacturer);

  if (desc->product)
    free((void*) desc->product);

  free(desc);
}

/**
 * @brief Get a list of the UVC devices attached to the system
 * @ingroup device
 *
 * @note Free the list with uvc_free_device_list when you're done.
 *
 * @param ctx UVC context in which to list devices
 * @param list List of uvc_device structures
 * @return Error if unable to list devices, else SUCCESS
 */
uvc_error_t uvc_get_device_list(
    uvc_context_t *ctx,
    uvc_device_t ***list) {
  uvc_error_t ret;
  struct libusb_device **usb_dev_list;
  struct libusb_device *usb_dev;
  size_t num_usb_devices;

  uvc_device_t **list_internal;
  size_t num_uvc_devices;

  /* per device */
  int dev_idx;
  struct libusb_device_handle *usb_devh;
  struct libusb_config_descriptor *config;
  uint8_t got_interface;

  /* per interface */
  int interface_idx;
  const struct libusb_interface *interface;

  /* per altsetting */
  int altsetting_idx;
  const struct libusb_interface_descriptor *if_desc;

  num_usb_devices = libusb_get_device_list(ctx->usb_ctx, &usb_dev_list);

  if (num_usb_devices < 0) {
    return UVC_ERROR_IO;
  }

  list_internal = malloc(sizeof(*list_internal));
  *list_internal = NULL;

  num_uvc_devices = 0;
  dev_idx = -1;

  while ((usb_dev = usb_dev_list[++dev_idx]) != NULL) {
    usb_devh = NULL;
    got_interface = 0;

    if (libusb_get_config_descriptor(usb_dev, 0, &config) != 0)
      continue;

    for (interface_idx = 0;
	 !got_interface && interface_idx < config->bNumInterfaces;
	 ++interface_idx) {
      interface = &config->interface[interface_idx];

      for (altsetting_idx = 0;
	   !got_interface && altsetting_idx < interface->num_altsetting;
	   ++altsetting_idx) {
	if_desc = &interface->altsetting[altsetting_idx];

	/* Video, Streaming */
	if (if_desc->bInterfaceClass == 14 && if_desc->bInterfaceSubClass == 2) {
	  got_interface = 1;
	}
      }
    }

    libusb_free_config_descriptor(config);

    if (got_interface) {
      uvc_device_t *uvc_dev = malloc(sizeof(*uvc_dev));
      uvc_dev->ctx = ctx;
      uvc_dev->ref = 0;
      uvc_dev->usb_dev = usb_dev;
      uvc_ref_device(uvc_dev);

      num_uvc_devices++;
      list_internal = realloc(list_internal, (num_uvc_devices + 1) * sizeof(*list_internal));

      list_internal[num_uvc_devices - 1] = uvc_dev;
      list_internal[num_uvc_devices] = NULL;
    }
  }

  libusb_free_device_list(usb_dev_list, 1);

  *list = list_internal;

  return UVC_SUCCESS;
}

/**
 * @brief Frees a list of device structures created with uvc_get_device_list.
 * @ingroup device
 *
 * @param list Device list to free
 * @param unref_devices Decrement the reference counter for each device
 * in the list, and destroy any entries that end up with zero references
 */
void uvc_free_device_list(uvc_device_t **list, uint8_t unref_devices) {
  uvc_device_t *dev;
  int dev_idx = 0;

  if (unref_devices) {
    while ((dev = list[dev_idx++]) != NULL) {
      uvc_unref_device(dev);
    }
  }

  free(list);
}

/**
 * @brief Increment the reference count for a device
 * @ingroup device
 *
 * @param dev Device to reference
 */
void uvc_ref_device(uvc_device_t *dev) {
  dev->ref++;
  libusb_ref_device(dev->usb_dev);
}

/**
 * @brief Decrement the reference count for a device
 * @ingropu device
 * @note If the count reaches zero, the device will be discarded
 *
 * @param dev Device to unreference
 */
void uvc_unref_device(uvc_device_t *dev) {
  libusb_unref_device(dev->usb_dev);
  dev->ref--;

  if (dev->ref == 0)
    free(dev);
}

/** @internal
 * Claim UVC interfaces, detaching kernel driver if necessary
 * @ingroup device
 *
 * @todo Use the right interface numbers...
 * @todo Make a note of detachment so we can reattach later
 *
 * @param devh UVC device handle
 */
uvc_error_t uvc_claim_ifs(uvc_device_handle_t *devh) {
  uvc_error_t ret;
 
  /* VideoControl interface */
  if (libusb_kernel_driver_active(devh->usb_devh, 0)) {
    ret = libusb_detach_kernel_driver(devh->usb_devh, 0);

    if (ret != UVC_SUCCESS)
      return ret;
  }

  ret = libusb_claim_interface(devh->usb_devh, 0);

  /* VideoStreaming interface */

  if (libusb_kernel_driver_active(devh->usb_devh, 1)) {
    ret = libusb_detach_kernel_driver(devh->usb_devh, 1);

    if (ret != UVC_SUCCESS)
      return ret;
  }

  if (ret != UVC_SUCCESS)
    return ret;

  ret = libusb_claim_interface(devh->usb_devh, 1);

  return ret;
}

/** @internal
 * Release UVC interfaces
 * @ingroup device
 *
 * @todo Use the right interface numbers
 * @todo Reattach kernel drivers
 *
 * @param devh UVC device handle
 */
void uvc_release_ifs(uvc_device_handle_t *devh) {
  libusb_release_interface(devh->usb_devh, 0);
  libusb_release_interface(devh->usb_devh, 1);
}

/** @internal
 * Find a device's VideoControl interface and process its descriptor
 * @ingroup device
 */
uvc_error_t uvc_scan_control(uvc_device_t *dev, uvc_device_info_t *info) {
  const struct libusb_interface_descriptor *if_desc;
  uvc_error_t parse_ret, ret;
  int interface_idx;
  const unsigned char *buffer;
  size_t buffer_left, block_size;

  ret = UVC_SUCCESS;
  if_desc = NULL;

  for (interface_idx = 0; interface_idx < info->config->bNumInterfaces; ++interface_idx) {
    if_desc = &info->config->interface[interface_idx].altsetting[0];

    if (if_desc->bInterfaceClass == 14 && if_desc->bInterfaceSubClass == 1) // Video, Control
      break;

    if_desc = NULL;
  }

  if (if_desc == NULL)
    return UVC_ERROR_INVALID_DEVICE;

  if (if_desc->bNumEndpoints != 0) {
    info->ctrl_if.bEndpointAddress = if_desc->endpoint[0].bEndpointAddress;
  }

  buffer = if_desc->extra;
  buffer_left = if_desc->extra_length;

  while (buffer_left >= 3) { // parseX needs to see buf[0,2] = length,type
    block_size = buffer[0];
    parse_ret = uvc_parse_vc(dev, info, buffer, block_size);

    if (parse_ret != UVC_SUCCESS) {
      ret = parse_ret;
      break;
    }

    buffer_left -= block_size;
    buffer += block_size;
  }

  return ret;
}

/** @internal
 * @brief Parse a VideoControl header.
 * @ingroup device
 */
uvc_error_t uvc_parse_vc_header(uvc_device_t *dev,
				uvc_device_info_t *info,
				const unsigned char *block, size_t block_size) {
  size_t i;
  uvc_error_t scan_ret, ret = UVC_SUCCESS;

  /*
  int uvc_version;
  uvc_version = (block[4] >> 4) * 1000 + (block[4] & 0x0f) * 100
    + (block[3] >> 4) * 10 + (block[3] & 0x0f);
  */

  info->ctrl_if.bcdUVC = SW_TO_SHORT(&block[3]);

  switch (info->ctrl_if.bcdUVC) {
  case 0x0100:
  case 0x010a:
  case 0x0110:
    break;
  default:
    return UVC_ERROR_NOT_SUPPORTED;
  }

  for (i = 12; i < block_size; ++i) {
    scan_ret = uvc_scan_streaming(dev, info, block[i]);
    if (scan_ret != UVC_SUCCESS) {
      ret = scan_ret;
      break;
    }
  }

  return ret;
}

/** @internal
 * @brief Parse a VideoControl input terminal.
 * @ingroup device
 */
uvc_error_t uvc_parse_vc_input_terminal(uvc_device_t *dev,
					uvc_device_info_t *info,
					const unsigned char *block, size_t block_size) {
  uvc_input_terminal_t *term;
  size_t i;

  /* only supporting camera-type input terminals */
  if (SW_TO_SHORT(&block[4]) != UVC_ITT_CAMERA)
    return;

  term = calloc(1, sizeof(*term));

  term->bTerminalID = block[3];
  term->wTerminalType = SW_TO_SHORT(&block[4]);
  term->wObjectiveFocalLengthMin = SW_TO_SHORT(&block[8]);
  term->wObjectiveFocalLengthMax = SW_TO_SHORT(&block[10]);
  term->wOcularFocalLength = SW_TO_SHORT(&block[12]);

  for (i = 14 + block[14]; i >= 15; --i)
    term->bmControls = block[i] + (term->bmControls << 8);

  DL_APPEND(info->ctrl_if.input_term_descs, term);

  return UVC_SUCCESS;
}

/** @internal
 * @brief Parse a VideoControl processing unit.
 * @ingroup device
 */
uvc_error_t uvc_parse_vc_processing_unit(uvc_device_t *dev,
					 uvc_device_info_t *info,
					 const unsigned char *block, size_t block_size) {
  uvc_processing_unit_t *unit;
  size_t i;

  unit = calloc(1, sizeof(*unit));
  unit->bUnitID = block[3];
  unit->bSourceID = block[4];
  
  for (i = 7 + block[7]; i >= 8; --i)
    unit->bmControls = block[i] + (unit->bmControls << 8);
  
  DL_APPEND(info->ctrl_if.processing_unit_descs, unit);

  return UVC_SUCCESS;
}

/** @internal
 * @brief Parse a VideoControl extension unit.
 * @ingroup device
 */
uvc_error_t uvc_parse_vc_extension_unit(uvc_device_t *dev,
					uvc_device_info_t *info,
					const unsigned char *block, size_t block_size) {
  uvc_extension_unit_t *unit = calloc(1, sizeof(*unit));
  const uint8_t *start_of_controls;
  int size_of_controls, num_in_pins;
  int i;
  
  unit->bUnitID = block[3];
  memcpy(unit->guidExtensionCode, &block[4], 16);
  
  num_in_pins = block[21];
  size_of_controls = block[22 + num_in_pins];
  start_of_controls = &block[23 + num_in_pins];
  
  for (i = size_of_controls - 1; i >= 0; --i)
    unit->bmControls = start_of_controls[i] + (unit->bmControls << 8);
  
  DL_APPEND(info->ctrl_if.extension_unit_descs, unit);
  
  return UVC_SUCCESS;
}

/** @internal
 * Process a single VideoControl descriptor block
 * @ingroup device
 */
uvc_error_t uvc_parse_vc(
    uvc_device_t *dev,
    uvc_device_info_t *info,
    const unsigned char *block, size_t block_size) {
  int descriptor_subtype;
  uvc_error_t ret = UVC_SUCCESS;

  if (block[1] != 36) // not a CS_INTERFACE descriptor??
    return UVC_SUCCESS; // UVC_ERROR_INVALID_DEVICE;

  descriptor_subtype = block[2];

  switch (descriptor_subtype) {
  case UVC_VC_HEADER:
    ret = uvc_parse_vc_header(dev, info, block, block_size);
    break;
  case UVC_VC_INPUT_TERMINAL:
    ret = uvc_parse_vc_input_terminal(dev, info, block, block_size);
    break;
  case UVC_VC_OUTPUT_TERMINAL:
    break;
  case UVC_VC_SELECTOR_UNIT:
    break;
  case UVC_VC_PROCESSING_UNIT:
    ret = uvc_parse_vc_processing_unit(dev, info, block, block_size);
    break;
  case UVC_VC_EXTENSION_UNIT:
    ret = uvc_parse_vc_extension_unit(dev, info, block, block_size);
    break;
  default:
    ret = UVC_ERROR_INVALID_DEVICE;
  }

  return ret;
}

/** @internal
 * Process a VideoStreaming interface
 * @ingroup device
 */
uvc_error_t uvc_scan_streaming(uvc_device_t *dev,
			       uvc_device_info_t *info,
			       int interface_idx) {
  const struct libusb_interface_descriptor *if_desc;
  const unsigned char *buffer;
  size_t buffer_left, block_size;
  uvc_error_t ret, parse_ret;
  uvc_streaming_interface_t *stream_if;

  ret = UVC_SUCCESS;

  if_desc = &(info->config->interface[interface_idx].altsetting[0]);
  buffer = if_desc->extra;
  buffer_left = if_desc->extra_length;

  stream_if = calloc(1, sizeof(*stream_if));
  stream_if->parent = info;
  stream_if->bInterfaceNumber = if_desc->bInterfaceNumber;
  DL_APPEND(info->stream_ifs, stream_if);

  while (buffer_left >= 3) {
    block_size = buffer[0];
    parse_ret = uvc_parse_vs(dev, info, stream_if, buffer, block_size);

    if (parse_ret != UVC_SUCCESS) {
      ret = parse_ret;
      break;
    }

    buffer_left -= block_size;
    buffer += block_size;
  }

  return ret;
}

/** @internal
 * @brief Parse a VideoStreaming header block.
 * @ingroup device
 */
uvc_error_t uvc_parse_vs_input_header(uvc_streaming_interface_t *stream_if,
				      const unsigned char *block,
				      size_t block_size) {
  stream_if->bEndpointAddress = block[6] & 0x8f;
  stream_if->bTerminalLink = block[8];

  return UVC_SUCCESS;
}

/** @internal
 * @brief Parse a VideoStreaming uncompressed format block.
 * @ingroup device
 */
uvc_error_t uvc_parse_vs_format_uncompressed(uvc_streaming_interface_t *stream_if,
					     const unsigned char *block,
					     size_t block_size) {
  uvc_format_desc_t *format = calloc(1, sizeof(*format));

  format->parent = stream_if;
  format->bDescriptorSubtype = block[2];
  format->bFormatIndex = block[3];
  //format->bmCapabilities = block[4];
  //format->bmFlags = block[5];
  memcpy(format->guidFormat, &block[5], 16);
  format->bBitsPerPixel = block[21];
  format->bDefaultFrameIndex = block[22];
  format->bAspectRatioX = block[23];
  format->bAspectRatioY = block[24];
  format->bmInterlaceFlags = block[25];
  format->bCopyProtect = block[26];

  DL_APPEND(stream_if->format_descs, format);

  return UVC_SUCCESS;
}

/** @internal
 * @brief Parse a VideoStreaming uncompressed frame block.
 * @ingroup device
 */
uvc_error_t uvc_parse_vs_frame_uncompressed(uvc_streaming_interface_t *stream_if,
					    const unsigned char *block,
					    size_t block_size) {
  uvc_format_desc_t *format;
  uvc_frame_desc_t *frame;

  const unsigned char *p;
  int i;

  format = stream_if->format_descs->prev;
  frame = calloc(1, sizeof(*frame));

  frame->parent = format;

  frame->bDescriptorSubtype = block[2];
  frame->bFrameIndex = block[3];
  frame->bmCapabilities = block[4];
  frame->wWidth = block[5] + (block[6] << 8);
  frame->wHeight = block[7] + (block[8] << 8);
  frame->dwMinBitRate = DW_TO_INT(&block[9]);
  frame->dwMaxBitRate = DW_TO_INT(&block[13]);
  frame->dwMaxVideoFrameBufferSize = DW_TO_INT(&block[17]);
  frame->dwDefaultFrameInterval = DW_TO_INT(&block[21]);
  // frame->bFrameIntervalType = block[25];

  if (block[25] == 0) {
    frame->dwMinFrameInterval = DW_TO_INT(&block[26]);
    frame->dwMaxFrameInterval = DW_TO_INT(&block[30]);
    frame->dwFrameIntervalStep = DW_TO_INT(&block[34]);
  } else {
    frame->intervals = calloc(block[25] + 1, sizeof(frame->intervals[0]));
    p = &block[26];

    for (i = 0; i < block[25]; ++i) {
      frame->intervals[i] = DW_TO_INT(p);
      p += 4;
    }
    frame->intervals[block[25]] = 0;
  }

  DL_APPEND(format->frame_descs, frame);

  return UVC_SUCCESS;
}

/** @internal
 * Process a single VideoStreaming descriptor block
 * @ingroup device
 */
uvc_error_t uvc_parse_vs(
    uvc_device_t *dev,
    uvc_device_info_t *info,
    uvc_streaming_interface_t *stream_if,
    const unsigned char *block, size_t block_size) {
  uvc_error_t ret;
  int descriptor_subtype;

  ret = UVC_SUCCESS;
  descriptor_subtype = block[2];

  switch (descriptor_subtype) {
  case UVC_VS_INPUT_HEADER:
    ret = uvc_parse_vs_input_header(stream_if, block, block_size);
    break;
  case UVC_VS_FORMAT_UNCOMPRESSED:
    ret = uvc_parse_vs_format_uncompressed(stream_if, block, block_size);
    break;
  case UVC_VS_FRAME_UNCOMPRESSED:
    ret = uvc_parse_vs_frame_uncompressed(stream_if, block, block_size);
    break;
  default:
    /** @todo handle JPEG and maybe still frames or even DV... */
    break;
  }

  return ret;
}

/** @internal
 * @brief Free memory associated with a UVC device
 * @pre Streaming must be stopped, and threads must have died
 */
void uvc_free_devh(uvc_device_handle_t *devh) {
  if (devh->info)
    uvc_free_device_info(devh->info);

  if (devh->status_xfer)
    libusb_free_transfer(devh->status_xfer);

  /* free the frame data (exists if any frames came through) */
  if (devh->stream.frame.data)
    free(devh->stream.frame.data);
  
  free(devh);
}

/** @brief Close a device
 *
 * @ingroup device
 *
 * Ends any stream that's in progress.
 *
 * The device handle and frame structures will be invalidated.
 */
void uvc_close(uvc_device_handle_t *devh) {
  if (devh->streaming)
    uvc_stop_streaming(devh);

  uvc_release_ifs(devh);

  libusb_close(devh->usb_devh);

  DL_DELETE(devh->dev->ctx->open_devices, devh);

  uvc_unref_device(devh->dev);

  uvc_free_devh(devh);
}

/** @internal
 * @brief Get number of open devices
 */
size_t uvc_num_devices(uvc_context_t *ctx) {
  size_t count = 0;

  uvc_device_handle_t *devh;

  DL_FOREACH(ctx->open_devices, devh) {
    count++;
  }

  return count;
}

void uvc_process_status_xfer(uvc_device_handle_t *devh, struct libusb_transfer *transfer) {
  enum uvc_status_class status_class;
  uint8_t originator = 0, selector = 0, event = 0;
  enum uvc_status_attribute attribute = UVC_STATUS_ATTRIBUTE_UNKNOWN;
  void *data = NULL;
  size_t data_len = 0;

  printf("Got transfer of aLen = %d\n", transfer->actual_length);

  if (transfer->actual_length < 4) {
    printf("Short read of status update (%d bytes)\n", transfer->actual_length);
    return;
  }

  originator = transfer->buffer[1];

  switch (transfer->buffer[0] & 0x0f) {
  case 1: {  /* VideoControl interface */
    int found_entity = 0;
    struct uvc_input_terminal *input_terminal;
    struct uvc_processing_unit *processing_unit;

    if (transfer->actual_length < 5)
      return;

    event = transfer->buffer[2];
    selector = transfer->buffer[3];

    if (originator == 0)
      return;  /* @todo VideoControl virtual entity interface updates */

    if (event != 0)
      return;

    printf("bSelector: %d\n", selector);

    DL_FOREACH(devh->info->ctrl_if.input_term_descs, input_terminal) {
      if (input_terminal->bTerminalID == originator) {
        status_class = UVC_STATUS_CLASS_CONTROL_CAMERA;
        found_entity = 1;
        break;
      }
    }

    if (!found_entity) {
      DL_FOREACH(devh->info->ctrl_if.processing_unit_descs, processing_unit) {
        if (processing_unit->bUnitID == originator) {
          status_class = UVC_STATUS_CLASS_CONTROL_PROCESSING;
          found_entity = 1;
          break;
        }
      }
    }

    if (!found_entity) {
      fprintf(stderr, "uvc: Got status update for unknown VideoControl entity %d\n",
              originator);
      return;
    }

    attribute = transfer->buffer[4];
    data = transfer->buffer + 5;
    data_len = transfer->actual_length - 5;
    break;
  }
  case 2:  /* VideoStreaming interface */
    return;  /* @todo VideoStreaming updates */
  }

  devh->status_cb(status_class,
                  event,
                  selector,
                  attribute,
                  data, data_len,
                  devh->status_user_ptr);
}

/** @internal
 * @brief Process asynchronous status updates from the device.
 */
void _uvc_status_callback(struct libusb_transfer *transfer) {
  uvc_device_handle_t *devh = (uvc_device_handle_t *) transfer->user_data;

  switch (transfer->status) {
  case LIBUSB_TRANSFER_ERROR:
  case LIBUSB_TRANSFER_CANCELLED:
  case LIBUSB_TRANSFER_NO_DEVICE:
    return;
  case LIBUSB_TRANSFER_COMPLETED:
    uvc_process_status_xfer(devh, transfer);
    break;
  }

  libusb_submit_transfer(transfer);
}

/** @brief Set a callback function to receive status updates
 *
 * @ingroup device
 */
void uvc_set_status_callback(uvc_device_handle_t *devh,
                             uvc_status_callback_t cb,
                             void *user_ptr) {
  devh->status_cb = cb;
  devh->status_user_ptr = user_ptr;
}
