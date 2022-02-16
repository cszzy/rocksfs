#pragma once

#include <stdlib.h>
#include <stdio.h>

#include "common/common.h"
#include "util/cJSON.h"

namespace metafs {
//only support int and string
struct conf_parser
{
	const char *name;
	size_t off;
	int type; //json type
	const char *default_value;
};

static inline void parse_from_str(void *conf, const char *str, struct conf_parser *cparser)
{
	cJSON *conf_json = cJSON_Parse((const char *)str);
	p_assert(conf_json, "json wrong\n");

	for (int i = 0; cparser[i].name; i++)
	{
		cJSON *obj;
		struct conf_parser *cpr = &cparser[i];
		obj = cJSON_GetObjectItem(conf_json, cpr->name);
		if (!obj || (obj->type & 0xFF) != cpr->type)
		{
			p_assert(cpr->default_value, "no default value");
			p_info("%s = %s     (default value)", cpr->name, cpr->default_value);
			if (cpr->type == cJSON_Number)
				setoffval(conf, cpr->off, int) = atoi(cpr->default_value);
			else if (cpr->type == cJSON_Number)
				setoffval(conf, cpr->off, char *) = strdump(cpr->default_value);
			else
				setoffval(conf, cpr->off, char **) = NULL;
		}
		else
		{
			if (cJSON_IsNumber(obj))
			{
				p_info("%s = %d", cpr->name, obj->valueint);
				setoffval(conf, cpr->off, int) = obj->valueint;
			}
			else if (cJSON_IsString(obj))
			{
				p_info("%s = %s", cpr->name, obj->valuestring);
				setoffval(conf, cpr->off, char *) = strdump(obj->valuestring);
			}
			else if (cJSON_IsArray(obj))
			{
				int len = cJSON_GetArraySize(obj), i = 0;
				struct cJSON *ele;
				char **arr = setoffval(conf, cpr->off, char **) = (char **)safe_alloc((1 + len) * sizeof(char *), true);
				cJSON_ArrayForEach(ele, obj)
				{
					p_assert(cJSON_IsString(ele), "not string");
					arr[i++] = strdump(ele->valuestring);
				}
			}
		}
	}
	cJSON_Delete(conf_json);
}

static inline void parse_from_file(void *conf, const char *fn, struct conf_parser *cparser)
{
	void *buf;
	size_t flen, rlen = 0;
	FILE *fp = NULL;
	fp = fopen(fn, "r");
	p_assert(fp, "file no found");

	fseek(fp, 0L, SEEK_END);
	flen = ftell(fp);
	fseek(fp, 0L, SEEK_SET);
	buf = safe_alloc(flen, true);
	while (rlen < flen)
	{
		rlen += fread((char *)buf + rlen, 1, flen - rlen, fp);
	}
	fclose(fp);
	parse_from_str(conf, (char *)buf, cparser);
	free(buf);
}

}
