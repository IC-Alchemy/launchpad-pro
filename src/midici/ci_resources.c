/*
 * MIDI-CI Property Exchange resources: const JSON, lives in flash (.rodata),
 * never copied to RAM. Identity values match ci_identity.h (decimal bytes, as
 * the PE DeviceInfo schema wants). The DeviceInfo serialNumber is the Product
 * Instance ID, which is only known at run time: the responder appends it.
 */
#include "midici/ci_resources.h"

const char ci_resource_list_json[] =
	"[{\"resource\":\"ResourceList\"},{\"resource\":\"DeviceInfo\"}]";

const char ci_device_info_json[] =
	"{\"manufacturerId\":[0,32,41],\"manufacturer\":\"Novation\","
	"\"familyId\":[81,0],\"family\":\"Launchpad\","
	"\"modelId\":[0,0],\"model\":\"Launchpad Pro\","
	"\"versionId\":[0,1,0,0],\"version\":\"1.0.0.0\"}";

const uint16_t ci_resource_list_json_len = sizeof ci_resource_list_json - 1;
const uint16_t ci_device_info_json_len = sizeof ci_device_info_json - 1;
