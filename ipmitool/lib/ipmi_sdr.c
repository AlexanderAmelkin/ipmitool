/*
 * Copyright (c) 2003 Sun Microsystems, Inc.  All Rights Reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistribution of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * Redistribution in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * Neither the name of Sun Microsystems, Inc. or the names of
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * 
 * This software is provided "AS IS," without a warranty of any kind.
 * ALL EXPRESS OR IMPLIED CONDITIONS, REPRESENTATIONS AND WARRANTIES,
 * INCLUDING ANY IMPLIED WARRANTY OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE OR NON-INFRINGEMENT, ARE HEREBY EXCLUDED.
 * SUN MICROSYSTEMS, INC. ("SUN") AND ITS LICENSORS SHALL NOT BE LIABLE
 * FOR ANY DAMAGES SUFFERED BY LICENSEE AS A RESULT OF USING, MODIFYING
 * OR DISTRIBUTING THIS SOFTWARE OR ITS DERIVATIVES.  IN NO EVENT WILL
 * SUN OR ITS LICENSORS BE LIABLE FOR ANY LOST REVENUE, PROFIT OR DATA,
 * OR FOR DIRECT, INDIRECT, SPECIAL, CONSEQUENTIAL, INCIDENTAL OR
 * PUNITIVE DAMAGES, HOWEVER CAUSED AND REGARDLESS OF THE THEORY OF
 * LIABILITY, ARISING OUT OF THE USE OF OR INABILITY TO USE THIS SOFTWARE,
 * EVEN IF SUN HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGES.
 * 
 * You acknowledge that this software is not designed or intended for use
 * in the design, construction, operation or maintenance of any nuclear
 * facility.
 */

#include <string.h>
#include <math.h>

#include <ipmitool/ipmi.h>
#include <ipmitool/ipmi_sdr.h>
#include <ipmitool/ipmi_entity.h>

extern int verbose;

/* convert unsigned value to 2's complement signed */
int utos(unsigned val, unsigned bits)
{
	int x = pow(10, bits-1);
	if (val & x) {
		x = pow(2, bits-1);
		return -((~val & (x-1))+1);
	}
	else return val;
}

static float
sdr_convert_sensor_reading(struct sdr_record_full_sensor * sensor, unsigned char val)
{
	int m, b, k1, k2;

	m  = __TO_M(sensor->mtol);
	b  = __TO_B(sensor->bacc);
	k1 = __TO_B_EXP(sensor->bacc);
	k2 = __TO_R_EXP(sensor->bacc);

	return (float)(((m * val) + (b * pow(10, k1))) * pow(10, k2));
}

#define GET_SENSOR_READING	0x2d
#define GET_SENSOR_FACTORS      0x23
#define GET_SENSOR_THRES	0x27
#define GET_SENSOR_TYPE		0x2f

static inline struct ipmi_rs *
ipmi_sdr_get_sensor_reading(struct ipmi_intf * intf, unsigned char sensor)
{
	struct ipmi_rs * rsp;
	struct ipmi_rq req;

	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_SE;
	req.msg.cmd = GET_SENSOR_READING;
	req.msg.data = &sensor;
	req.msg.data_len = sizeof(sensor);

	rsp = intf->sendrecv(intf, &req);

	return rsp;
}

static const char *
ipmi_sdr_get_status(unsigned char stat)
{
	/* cr = critical
	 * nc = non-critical
	 * us = unspecified
	 * nr = non-recoverable
	 * ok = ok
	 */
	if (stat & (SDR_SENSOR_STAT_LO_NR | SDR_SENSOR_STAT_HI_NR))
		return "nr";
	else if (stat & (SDR_SENSOR_STAT_LO_CR | SDR_SENSOR_STAT_HI_CR))
		return "cr";	
	else if (stat & (SDR_SENSOR_STAT_LO_NC | SDR_SENSOR_STAT_HI_NC))
		return "nc";
	else
		return "ok";
}

static struct sdr_get_rs *
ipmi_sdr_get_header(struct ipmi_intf * intf, unsigned short reserve_id, unsigned short record_id)
{
	struct ipmi_rq req;
	struct ipmi_rs * rsp;
	struct sdr_get_rq sdr_rq;
	static struct sdr_get_rs sdr_rs;

	memset(&sdr_rq, 0, sizeof(sdr_rq));
	sdr_rq.reserve_id = reserve_id;
	sdr_rq.id = record_id;
	sdr_rq.offset = 0;
	sdr_rq.length = 5;	/* only get the header */

	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_STORAGE;
	req.msg.cmd = GET_SDR;
	req.msg.data = (unsigned char *)&sdr_rq;
	req.msg.data_len = sizeof(sdr_rq);

	rsp = intf->sendrecv(intf, &req);
	if (!rsp || !rsp->data_len) {
		printf("Error getting SDR record id 0x%04x\n", record_id);
		return NULL;
	}

	if (verbose > 1)
		printf("SDR Record ID   : 0x%04x\n", record_id);

	memcpy(&sdr_rs, rsp->data, sizeof(sdr_rs));

	if (sdr_rs.length == 0) {
		printf("Error in SDR record id 0x%04x: invalid length %d\n",
		       record_id, sdr_rs.length);
		return NULL;
	}

	if (verbose > 1) {
		printf("SDR record type : %d\n", sdr_rs.type);
		printf("SDR record next : %d\n", sdr_rs.next);
		printf("SDR record bytes: %d\n", sdr_rs.length);
	}

	return &sdr_rs;
}

static struct sdr_record_compact_sensor *
ipmi_sdr_get_entry_02(struct ipmi_intf * intf, unsigned short reserve_id, unsigned short record_id, int len)
{
	struct ipmi_rq req;
	struct ipmi_rs * rsp;
	struct sdr_get_rq sdr_rq;
	struct sdr_record_compact_sensor * sensor;
	unsigned char data[256];
	int i;

	memset(&sdr_rq, 0, sizeof(sdr_rq));
	sdr_rq.reserve_id = reserve_id;
	sdr_rq.id = record_id;
	sdr_rq.offset = 0;

	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_STORAGE;
	req.msg.cmd = GET_SDR;
	req.msg.data = (unsigned char *)&sdr_rq;
	req.msg.data_len = sizeof(sdr_rq);

	/* read SDR record with partial (30 byte) reads
	 * because a full read (0xff) exceeds the maximum
	 * transport buffer size.  (completion code 0xca)
	 */
	memset(data, 0, sizeof(data));
	for (i=0; i<len; i+=GET_SDR_MAX_LEN) {
		sdr_rq.length = (len-i < GET_SDR_MAX_LEN) ? len-i : GET_SDR_MAX_LEN;
		sdr_rq.offset = i+5; /* 5 header bytes */
		if (verbose > 1)
			printf("getting %d bytes from SDR at offset %d\n",
			       sdr_rq.length, sdr_rq.offset);
		rsp = intf->sendrecv(intf, &req);
		if (rsp && rsp->data)
			memcpy(data+i, rsp->data+2, sdr_rq.length);
	}

	sensor = malloc(sizeof(*sensor));
	memcpy(sensor, data, sizeof(*sensor));

	if (verbose > 1) {
		printbuf(data, len, "SDR Entry");

		printf("keys.owner_id:   0x%x\n", sensor->keys.owner_id);
		printf("keys.lun:        0x%x\n", sensor->keys.lun);
		printf("keys.channel:    0x%x\n", sensor->keys.channel);
		printf("keys.sensor_num: 0x%x\n", sensor->keys.sensor_num);
		
		printf("entity:          %d.%d\n", sensor->entity.id, sensor->entity.instance);
		
		printf("entity.id:       %s\n", val2str(sensor->entity.id, entity_id_vals));
		printf("entity.instance: %d\n", sensor->entity.instance);
		printf("entity.logical:  %d\n", sensor->entity.logical);
		
		printf("sensor unit.pct: 0x%x\n", sensor->unit.pct);
		printf("sensor unit.rate: 0x%x\n", sensor->unit.rate);
		printf("sensor unit.analog: 0x%x\n", sensor->unit.analog);
		printf("sensor unit.modifier: 0x%x\n", sensor->unit.modifier);
		printf("sensor unit.type.base: 0x%x\n", sensor->unit.type.base);
		printf("sensor unit.type.modifier: 0x%x\n", sensor->unit.type.modifier);
		
		printf("sensor.type: 0x%02x\n", sensor->sensor.type);
		printf("event_type:  0x%02x\n", sensor->event_type);
		
		printf("sensor id code: 0x%x\n", sensor->id_code);

		if (sensor->id_code)
			printf("sensor id: %s\n", sensor->id_string);
	}

	return sensor;
}

static struct sdr_record_full_sensor *
ipmi_sdr_get_entry_01(struct ipmi_intf * intf, unsigned short reserve_id, unsigned short record_id, int len)
{
	struct ipmi_rq req;
	struct ipmi_rs * rsp;
	struct sdr_get_rq sdr_rq;
	struct sdr_record_full_sensor * sensor;
	unsigned char data[256];
	int i;

	memset(&sdr_rq, 0, sizeof(sdr_rq));
	sdr_rq.reserve_id = reserve_id;
	sdr_rq.id = record_id;
	sdr_rq.offset = 0;

	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_STORAGE;
	req.msg.cmd = GET_SDR;
	req.msg.data = (unsigned char *)&sdr_rq;
	req.msg.data_len = sizeof(sdr_rq);

	/* read SDR record with partial (30 byte) reads
	 * because a full read (0xff) exceeds the maximum
	 * transport buffer size.  (completion code 0xca)
	 */
	memset(data, 0, sizeof(data));
	for (i=0; i<len; i+=GET_SDR_MAX_LEN) {
		sdr_rq.length = (len-i < GET_SDR_MAX_LEN) ? len-i : GET_SDR_MAX_LEN;
		sdr_rq.offset = i+5; /* 5 header bytes */
		if (verbose > 1)
			printf("getting %d bytes from SDR at offset %d\n",
			       sdr_rq.length, sdr_rq.offset);
		rsp = intf->sendrecv(intf, &req);
		if (rsp && rsp->data)
			memcpy(data+i, rsp->data+2, sdr_rq.length);
	}

	sensor = malloc(sizeof(*sensor));
	memcpy(sensor, data, sizeof(*sensor));

	if (verbose > 1) {
		printbuf(data, len, "SDR Entry");

		printf("keys.owner_id:   0x%x\n", sensor->keys.owner_id);
		printf("keys.lun:        0x%x\n", sensor->keys.lun);
		printf("keys.channel:    0x%x\n", sensor->keys.channel);
		printf("keys.sensor_num: 0x%x\n", sensor->keys.sensor_num);
		
		printf("entity:          %d.%d\n", sensor->entity.id, sensor->entity.instance);
		
		printf("entity.id:       %s\n", val2str(sensor->entity.id, entity_id_vals));
		printf("entity.instance: %d\n", sensor->entity.instance);
		printf("entity.logical:  %d\n", sensor->entity.logical);
		
		printf("sensor unit.pct: 0x%x\n", sensor->unit.pct);
		printf("sensor unit.rate: 0x%x\n", sensor->unit.rate);
		printf("sensor unit.analog: 0x%x\n", sensor->unit.analog);
		printf("sensor unit.modifier: 0x%x\n", sensor->unit.modifier);
		printf("sensor unit.type.base: 0x%x\n", sensor->unit.type.base);
		printf("sensor unit.type.modifier: 0x%x\n", sensor->unit.type.modifier);
		
		printf("sensor linearization: 0x%x\n", sensor->linearization);
		
		printf("sensor tolerance: 0x%x\n", __TO_TOL(sensor->mtol));
		printf("sensor M: 0x%x\n", __TO_M(sensor->mtol));
		printf("sensor B: 0x%x\n", __TO_B(sensor->bacc));
		printf("sensor B exp: %d\n", __TO_B_EXP(sensor->bacc));
		printf("sensor R exp: %d\n", __TO_R_EXP(sensor->bacc));
		printf("sensor accuracy: 0x%x\n", __TO_ACC(sensor->bacc));
		printf("sensor accuracy exp: 0x%x\n", __TO_ACC_EXP(sensor->bacc));
		
		printf("sensor.type: 0x%02x\n", sensor->sensor.type);
		printf("event_type:  0x%02x\n", sensor->event_type);
		
		printf("sensor min=0x%x max=0x%x\n", sensor->sensor_min, sensor->sensor_max);
		printf("sensor id code: 0x%x\n", sensor->id_code);

		printf("Nominal Reading                 : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->nominal_read));
		printf("Normal Minimum Reading          : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->normal_min));
		printf("Normal Maximum Reading          : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->normal_max));
		printf("Upper non-recoverable Threshold : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.upper.non_recover));
		printf("Upper critical Threshold        : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.upper.critical));
		printf("Upper non-critical Threshold    : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.upper.non_critical));
		printf("Lower non-recoverable Threshold : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.lower.non_recover));
		printf("Lower critical Threshold        : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.lower.critical));
		printf("Lower non-critical Threshold    : %.3f\n",
		       sdr_convert_sensor_reading(sensor, sensor->threshold.lower.non_critical));
		
		if (sensor->id_code)
			printf("sensor id: %s\n", sensor->id_string);
	}

	return sensor;
}

static void
ipmi_sdr_print_sensors(struct ipmi_intf * intf, int do_unit)
{
	struct ipmi_rs * rsp;
	struct ipmi_rq req;
	struct sdr_repo_info_rs sdr_info;
	struct sdr_reserve_repo_rs sdr_reserve;
	struct sdr_get_rs * header;
	struct sdr_record_full_sensor * sensor;

	int next = 0, i = 0, total, validread;
	unsigned short reservation;
	float val;
	char sval[16], unitstr[16];
	char desc[17];

	if (verbose)
		printf("Querying SDR for sensor list\n");

	/* get sdr repository info */
	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_STORAGE;
	req.msg.cmd = GET_SDR_REPO_INFO;

	rsp = intf->sendrecv(intf, &req);
	if (!rsp || !rsp->data_len)
		return;
	memcpy(&sdr_info, rsp->data, sizeof(sdr_info));

	/* byte 1 is SDR version, should be 51h */
	if (sdr_info.version != 0x51) {
		printf("SDR repository version mismatch!\n");
		return;
	}
	total = sdr_info.count;
	if (verbose > 1) {
		printf("SDR free space: %d\n", sdr_info.free);
		printf("SDR records: %d\n", total);
	}

	/* obtain reservation ID */
	memset(&req, 0, sizeof(req));
	req.msg.netfn = IPMI_NETFN_STORAGE;
	req.msg.cmd = GET_SDR_RESERVE_REPO;
	rsp = intf->sendrecv(intf, &req);
	if (!rsp || !rsp->data_len)
		return;
	memcpy(&sdr_reserve, rsp->data, sizeof(sdr_reserve));
	reservation = sdr_reserve.reserve_id;
	if (verbose > 1)
		printf("SDR reserveration ID %04x\n", reservation);

	while (next < total) {
		validread = 1;
		i = 0;

		header = ipmi_sdr_get_header(intf, reservation, next);
		if (!header)
			break;

		if (header->type == SDR_RECORD_TYPE_COMPACT_SENSOR) {
			struct sdr_record_compact_sensor * s;
			s = ipmi_sdr_get_entry_02(intf, reservation, next, header->length);
			next = header->next;
			free(s);
			continue;
		}

		if (header->type != SDR_RECORD_TYPE_FULL_SENSOR) {
			if (verbose > 1)
				printf("Invalid SDR type 0x%02x\n", header->type);
			next = header->next;
			continue;
		}

		sensor = ipmi_sdr_get_entry_01(intf, reservation, next, header->length);
		next = header->next;

		/* only handle linear sensors (for now) */
		if (sensor->linearization) {
			printf("non-linear!\n");
			continue;
		}

		memset(desc, 0, sizeof(desc));
		memcpy(desc, sensor->id_string, 16);

		rsp = ipmi_sdr_get_sensor_reading(intf, sensor->keys.sensor_num);
		if (!rsp || rsp->ccode) {
			if (rsp && rsp->ccode == 0xcb) {
				/* sensor not found */
				val = 0.0;
				validread = 0;
			} else {
				printf("Error reading sensor: %s\n",
				       val2str(rsp->ccode, completion_code_vals));
				continue;
			}
		} else {
			/* convert RAW reading into units */
			val = rsp->data[0] ? sdr_convert_sensor_reading(sensor, rsp->data[0]) : 0;
		}

		if (do_unit && validread) {
			memset(unitstr, 0, sizeof(unitstr));
			/* determine units with possible modifiers */
			switch (sensor->unit.modifier) {
			case 2:
				i += snprintf(unitstr, sizeof(unitstr), "%s * %s",
					      unit_desc[sensor->unit.type.base],
					      unit_desc[sensor->unit.type.modifier]);
				break;
			case 1:
				i += snprintf(unitstr, sizeof(unitstr), "%s/%s",
					      unit_desc[sensor->unit.type.base],
					      unit_desc[sensor->unit.type.modifier]);
				break;
			case 0:
			default:
				i += snprintf(unitstr, sizeof(unitstr), "%s",
					      unit_desc[sensor->unit.type.base]);
				break;
			}
		}

		if (!verbose) {
			/*
			 * print sensor name, reading, state
			 */
			if (csv_output)
				printf("%s,",
				       sensor->id_code ? desc : NULL);
			else
				printf("%-16s | ",
				       sensor->id_code ? desc : NULL);

			memset(sval, 0, sizeof(sval));
			if (validread) {
				i += snprintf(sval, sizeof(sval), "%.*f",
					      (val==(int)val) ? 0 : 3, val);
			} else {
				i += snprintf(sval, sizeof(sval), "no reading");
				i--;
			}
			printf("%s", sval);

			if (csv_output)
			    	printf(",");

			if (validread) {
				if (!csv_output)
					printf(" ");
				if (do_unit)
					printf("%s", unitstr);
			}

			if (csv_output)
				printf(",");
			else {
				for (; i<sizeof(sval); i++)
					printf(" ");
				printf(" | ");
			}

			printf("%s", ipmi_sdr_get_status(rsp->data[2]));
			printf("\n");
		}
		else {
			printf("Sensor  | %s (0x%x)\n",
			       sensor->id_code ? desc : NULL,
			       sensor->keys.sensor_num);
			printf("Entity  | %d.%d (%s)\n",
			       sensor->entity.id, sensor->entity.instance,
			       val2str(sensor->entity.id, entity_id_vals));
			if (validread)
				printf("Reading | %.*f %s\n",
				       (val==(int)val) ? 0 : 3, val, unitstr);
			else
				printf("Reading | not present\n");
			printf("Status  | %s\n",
			       ipmi_sdr_get_status(rsp->data[2]));
			printf("\n");
		}

		free(sensor);
	}
}

int ipmi_sdr_main(struct ipmi_intf * intf, int argc, char ** argv)
{
	if (!argc)
		ipmi_sdr_print_sensors(intf, 1);
	else if (!strncmp(argv[0], "help", 4))
		printf("SDR Commands:  list\n");
	else if (!strncmp(argv[0], "list", 4))
		ipmi_sdr_print_sensors(intf, 1);
	else
		printf("Invalid SDR command: %s\n", argv[0]);
	return 0;
}
