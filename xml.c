/*
 * libiio - Library for interfacing industrial I/O (IIO) devices
 *
 * Copyright (C) 2014 Analog Devices, Inc.
 * Author: Paul Cercueil <paul.cercueil@analog.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * */

#include "debug.h"
#include "iio-private.h"

#include <errno.h>
#include <libxml/tree.h>
#include <string.h>

/* 'input' must be in UTF-8 encoded, null terminated */
char * encode_xml_ndup(const char * input)
{
	char * out;

	out = (char *)xmlEncodeEntitiesReentrant(NULL, (const xmlChar *)input);

	return out;
}

static int add_attr_to_channel(struct iio_channel *chn, xmlNode *n)
{
	xmlAttr *attr;
	char *name = NULL, *filename = NULL;
	struct iio_channel_attr *attrs;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			name = iio_strdup((char *) attr->children->content);
		} else if (!strcmp((char *) attr->name, "filename")) {
			filename = iio_strdup((char *) attr->children->content);
		} else {
			IIO_WARNING("Unknown field \'%s\' in channel %s\n",
					attr->name, chn->id);
		}
	}

	if (!name) {
		IIO_ERROR("Incomplete attribute in channel %s\n", chn->id);
		goto err_free;
	}

	if (!filename) {
		filename = iio_strdup(name);
		if (!filename)
			goto err_free;
	}

	attrs = realloc(chn->attrs, (1 + chn->nb_attrs) *
			sizeof(struct iio_channel_attr));
	if (!attrs)
		goto err_free;

	attrs[chn->nb_attrs].filename = filename;
	attrs[chn->nb_attrs++].name = name;
	chn->attrs = attrs;
	return 0;

err_free:
	if (name)
		free(name);
	if (filename)
		free(filename);
	return -1;
}

static int add_attr_to_device(struct iio_device *dev, int bidx, xmlNode *n, enum iio_attr_type type)
{
	xmlAttr *attr;
	char *name = NULL;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			name = (char *) attr->children->content;
		} else {
			IIO_WARNING("Unknown field \'%s\' in device %s\n",
					attr->name, dev->id);
		}
	}

	if (!name) {
		IIO_ERROR("Incomplete attribute in device %s\n", dev->id);
		return -1;
	}

	switch(type) {
		case IIO_ATTR_TYPE_DEBUG:
			return add_iio_dev_attr(&dev->debug_attrs, name, " debug", dev->id);
		case IIO_ATTR_TYPE_DEVICE:
			return add_iio_dev_attr(&dev->attrs, name, " ", dev->id);
		case IIO_ATTR_TYPE_BUFFER:
			return add_iio_dev_attr(&dev->buffer_attrs[bidx], name, " buffer", dev->id);
		default:
			return -1;
	}
}

static void setup_scan_element(struct iio_channel *chn, xmlNode *n)
{
	xmlAttr *attr;

	for (attr = n->properties; attr; attr = attr->next) {
		const char *name = (const char *) attr->name,
		      *content = (const char *) attr->children->content;
		if (!strcmp(name, "index")) {
			char *end;
			long long value;

			errno = 0;
			value = strtoll(content, &end, 0);
			if (end == content || value < 0 || errno == ERANGE)
				return;
			chn->index = (long) value;
		} else if (!strcmp(name, "format")) {
			char e, s;
			if (strchr(content, 'X')) {
				iio_sscanf(content, "%ce:%c%u/%uX%u>>%u",
#ifdef _MSC_BUILD
					&e, (unsigned int)sizeof(e),
					&s, (unsigned int)sizeof(s),
#else
					&e, &s,
#endif
					&chn->format.bits,
					&chn->format.length,
					&chn->format.repeat,
					&chn->format.shift);
			} else {
				chn->format.repeat = 1;
				iio_sscanf(content, "%ce:%c%u/%u>>%u",
#ifdef _MSC_BUILD
					&e, (unsigned int)sizeof(e),
					&s, (unsigned int)sizeof(s),
#else
					&e, &s,
#endif
					&chn->format.bits,
					&chn->format.length,
					&chn->format.shift);
			}
			chn->format.is_be = e == 'b';
			chn->format.is_signed = (s == 's' || s == 'S');
			chn->format.is_fully_defined = (s == 'S' || s == 'U' ||
				chn->format.bits == chn->format.length);
		} else if (!strcmp(name, "scale")) {
			char *end;
			float value;

			errno = 0;
			value = strtof(content, &end);
			if (end == content || errno == ERANGE) {
				chn->format.with_scale = false;
				return;
			}

			chn->format.with_scale = true;
			chn->format.scale = value;
		} else {
			IIO_WARNING("Unknown attribute \'%s\' in <scan-element>\n",
					name);
		}
	}
}

static struct iio_channel * create_channel(struct iio_device *dev, xmlNode *n)
{
	xmlAttr *attr;
	struct iio_channel *chn = zalloc(sizeof(*chn));
	if (!chn)
		return NULL;

	chn->dev = dev;

	/* Set the default index value < 0 (== no index) */
	chn->index = -ENOENT;

	for (attr = n->properties; attr; attr = attr->next) {
		const char *name = (const char *) attr->name,
		      *content = (const char *) attr->children->content;
		if (!strcmp(name, "name")) {
			chn->name = iio_strdup(content);
		} else if (!strcmp(name, "id")) {
			chn->id = iio_strdup(content);
		} else if (!strcmp(name, "type")) {
			if (!strcmp(content, "output"))
				chn->is_output = true;
			else if (strcmp(content, "input"))
				IIO_WARNING("Unknown channel type %s\n", content);
		} else {
			IIO_WARNING("Unknown attribute \'%s\' in <channel>\n",
					name);
		}
	}

	if (!chn->id) {
		IIO_ERROR("Incomplete <attribute>\n");
		goto err_free_channel;
	}

	for (n = n->children; n; n = n->next) {
		if (!strcmp((char *) n->name, "attribute")) {
			if (add_attr_to_channel(chn, n) < 0)
				goto err_free_channel;
		} else if (!strcmp((char *) n->name, "scan-element")) {
			chn->is_scan_element = true;
			setup_scan_element(chn, n);
		} else if (strcmp((char *) n->name, "text")) {
			IIO_WARNING("Unknown children \'%s\' in <channel>\n",
					n->name);
			continue;
		}
	}

	iio_channel_init_finalize(chn);

	return chn;

err_free_channel:
	free_channel(chn);
	return NULL;
}

static int create_buffer_attrs(struct iio_device *dev, xmlNode *n)
{
	struct iio_dev_attrs *attrs;
	unsigned int idx;
	xmlAttr *attr;
	int ret = -ENOENT;

	for (attr = n->properties; attr; attr = attr->next) {
		const char *content = (const char *) attr->children->content;
		if (!strcmp((char *) attr->name, "index")) {
			long long value;
			char *end;
			errno = 0;
			value = strtoll(content, &end, 0);
			if (end == content || value < 0 || errno == ERANGE)
				return -ERANGE;
			idx = (unsigned int) value;
			ret = 0;
		} else {
			IIO_WARNING("Unknown field \'%s\' in device %s\n",
				    attr->name, dev->id);
		}
	}

	if (ret < 0) {
		IIO_ERROR("No index property found for buffer\n");
		return -1;
	}

	/* legacy buffer attributes were first */
	if (idx == 0 && dev->nb_buffers == 1)
		return 0;

	if (idx >= dev->nb_buffers) {
		attrs = realloc(dev->buffer_attrs, (idx + 1) * sizeof(*attrs));
		if (!attrs)
			return -ENOMEM;

		dev->nb_buffers = idx + 1;
		dev->buffer_attrs = attrs;
	}

	attrs = &dev->buffer_attrs[idx];
	if (attrs->num > 0) {
		/* old buffer-attributes beat us here */
		if (idx == 0)
			return 0;
		IIO_ERROR("Duplicate buffer element found for '%s' buffer%u\n",
			  dev->id, idx);
		return -EFAULT;
	}

	for (n = n->children; n; n = n->next) {
		if (!strcmp((char *) n->name, "attribute")) {
			ret = add_attr_to_device(dev, idx, n, IIO_ATTR_TYPE_BUFFER);
			if (ret < 0)
				return ret;
		} else {
			IIO_WARNING("Unknown children \'%s\' in <buffer%u>\n",
				    n->name, idx);
			continue;
		}
	}

	return 0;
}

static struct iio_device * create_device(struct iio_context *ctx, xmlNode *n)
{
	xmlAttr *attr;
	struct iio_device *dev = zalloc(sizeof(*dev));
	if (!dev)
		return NULL;

	dev->ctx = ctx;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "name")) {
			dev->name = iio_strdup(
					(char *) attr->children->content);
		} else if (!strcmp((char *) attr->name, "id")) {
			dev->id = iio_strdup((char *) attr->children->content);
		} else {
			IIO_WARNING("Unknown attribute \'%s\' in <device>\n",
					attr->name);
		}
	}

	if (!dev->id) {
		IIO_ERROR("Unable to read device ID\n");
		goto err_free_device;
	}

	for (n = n->children; n; n = n->next) {
		if (!strcmp((char *) n->name, "channel")) {
			struct iio_channel **chns,
					   *chn = create_channel(dev, n);
			if (!chn) {
				IIO_ERROR("Unable to create channel\n");
				goto err_free_device;
			}

			chns = realloc(dev->channels, (1 + dev->nb_channels) *
					sizeof(struct iio_channel *));
			if (!chns) {
				IIO_ERROR("Unable to allocate memory\n");
				free(chn);
				goto err_free_device;
			}

			chns[dev->nb_channels++] = chn;
			dev->channels = chns;
		} else if (!strcmp((char *) n->name, "buffer")) {
			int ret = create_buffer_attrs(dev, n);

			if (ret < 0)
				goto err_free_device;
		} else if (!strcmp((char *) n->name, "attribute")) {
			if (add_attr_to_device(dev, -1, n, IIO_ATTR_TYPE_DEVICE) < 0)
				goto err_free_device;
		} else if (!strcmp((char *) n->name, "debug-attribute")) {
			if (add_attr_to_device(dev, -1, n, IIO_ATTR_TYPE_DEBUG) < 0)
				goto err_free_device;
		} else if (!strcmp((char *) n->name, "buffer-attribute")) {
			/* initialized via new API, skip */
			if (dev->nb_buffers > 0)
				continue;
			dev->buffer_attrs = calloc(1, sizeof(*dev->buffer_attrs));
			if (!dev->buffer_attrs) {
				errno = ENOMEM;
				goto err_free_device;
			}
			dev->nb_buffers = 1;
			if (add_attr_to_device(dev, 0, n, IIO_ATTR_TYPE_BUFFER) < 0)
				goto err_free_device;
		} else if (strcmp((char *) n->name, "text")) {
			IIO_WARNING("Unknown children \'%s\' in <device>\n",
					n->name);
			continue;
		}
	}

	dev->words = (dev->nb_channels + 31) / 32;
	if (dev->words) {
		dev->mask = calloc(dev->words, sizeof(*dev->mask));
		if (!dev->mask) {
			errno = ENOMEM;
			goto err_free_device;
		}
	}

	return dev;

err_free_device:
	free_device(dev);
	return NULL;
}

static struct iio_context * xml_clone(const struct iio_context *ctx)
{
	return xml_create_context_mem(ctx->xml, strlen(ctx->xml));
}

static const struct iio_backend_ops xml_ops = {
	.clone = xml_clone,
};

static const struct iio_backend xml_backend = {
	.api_version = IIO_BACKEND_API_V1,
	.name = "xml",
	.uri_prefix = "xml:",
	.ops = &xml_ops,
};

static int parse_context_attr(struct iio_context *ctx, xmlNode *n)
{
	xmlAttr *attr;
	const char *name = NULL, *value = NULL;

	for (attr = n->properties; attr; attr = attr->next) {
		if (!strcmp((const char *) attr->name, "name")) {
			name = (const char *) attr->children->content;
		} else if (!strcmp((const char *) attr->name, "value")) {
			value = (const char *) attr->children->content;
		}
	}

	if (!name || !value)
		return -EINVAL;
	else
		return iio_context_add_attr(ctx, name, value);
}

static int iio_populate_xml_context_helper(struct iio_context *ctx, xmlNode *root)
{
	xmlNode *n;
	int err = -ENOMEM;

	for (n = root->children; n; n = n->next) {
		struct iio_device *dev;

		if (!strcmp((char *) n->name, "context-attribute")) {
			err = parse_context_attr(ctx, n);
			if (err)
				goto err_set_errno;
			else
				continue;
		} else if (strcmp((char *) n->name, "device")) {
			if (strcmp((char *) n->name, "text"))
				IIO_WARNING("Unknown children \'%s\' in "
						"<context>\n", n->name);
			continue;
		}

		dev = create_device(ctx, n);
		if (!dev) {
			IIO_ERROR("Unable to create device\n");
			goto err_set_errno;
		}

		err = iio_context_add_device(ctx, dev);
		if (err) {
			free(dev);
			goto err_set_errno;
		}
	}

	err = iio_context_init(ctx);
	if (err)
		goto err_set_errno;

	return 0;

err_set_errno:
	errno = -err;
	return err;
}

static struct iio_context * iio_create_xml_context_helper(xmlDoc *doc)
{
	const char *description = NULL;
	struct iio_context *ctx;
	xmlNode *root;
	xmlAttr *attr;

	root = xmlDocGetRootElement(doc);
	if (strcmp((char *) root->name, "context")) {
		IIO_ERROR("Unrecognized XML file\n");
		errno = EINVAL;
		return NULL;
	}

	for (attr = root->properties; attr; attr = attr->next) {
		if (!strcmp((char *) attr->name, "description"))
			description = (const char *)attr->children->content;
		else if (strcmp((char *) attr->name, "name"))
			IIO_WARNING("Unknown parameter \'%s\' in <context>\n",
					(char *) attr->children->content);
	}

	ctx = iio_context_create_from_backend(&xml_backend, description);
	if (!ctx)
		return NULL;

	if (iio_populate_xml_context_helper(ctx, root)) {
		iio_context_destroy(ctx);
		return NULL;
	}

	return ctx;
}

struct iio_context * xml_create_context(const char *xml_file)
{
	struct iio_context *ctx;
	xmlDoc *doc;

	LIBXML_TEST_VERSION;

	doc = xmlReadFile(xml_file, NULL, XML_PARSE_DTDVALID);
	if (!doc) {
		IIO_ERROR("Unable to parse XML file\n");
		errno = EINVAL;
		return NULL;
	}

	ctx = iio_create_xml_context_helper(doc);
	xmlFreeDoc(doc);
	return ctx;
}

struct iio_context * xml_create_context_mem(const char *xml, size_t len)
{
	struct iio_context *ctx;
	xmlDoc *doc;

	LIBXML_TEST_VERSION;

	doc = xmlReadMemory(xml, (int) len, NULL, NULL, XML_PARSE_DTDVALID);
	if (!doc) {
		IIO_ERROR("Unable to parse XML file\n");
		errno = EINVAL;
		return NULL;
	}

	ctx = iio_create_xml_context_helper(doc);
	xmlFreeDoc(doc);
	return ctx;
}
