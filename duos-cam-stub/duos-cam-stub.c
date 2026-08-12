// SPDX-License-Identifier: GPL-2.0
/*
 * A CSI-2 receiver that receives nothing.
 *
 * The SG2000 has no mainline CSI-2 receiver or ISP driver, so a sensor subdev
 * on this SoC has nowhere to attach: nothing ever calls
 * v4l2_device_register_subdev_nodes(), no /dev/v4l-subdev* appears, and every
 * part of the sensor driver above the chip-ID read is unreachable from
 * userspace. That is a bring-up problem, not a hardware one - the sensor is on
 * I2C and answers fine; it is only the V4L2 plumbing that is missing.
 *
 * This module is that plumbing and nothing else. It registers a v4l2_device,
 * waits for the sensor named by its own DT endpoint, and creates the subdev
 * device nodes when it arrives. It touches no receiver registers because there
 * are none to touch, and it moves no pixels: the MIPI lanes go nowhere. What it
 * buys is a /dev/v4l-subdev0 that v4l2-ctl can open, so formats, controls and
 * the streaming register sequences can be exercised against real silicon.
 *
 *   echo 1 > /sys/bus/platform/drivers/duos-cam-stub/<dev>/stream
 *
 * writes the sensor's full init table and stream-on sequence over I2C. Nothing
 * receives the result, so a clean return means the sensor accepted every write
 * and clocked its PLL up, not that a frame exists anywhere.
 *
 * It needs a DT node with a compatible and an endpoint pointing back at the
 * sensor - see duos-cam.dts, where the csi-sink stub node is the same node this
 * driver binds to.
 */

#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

struct duos_cam_stub {
	struct v4l2_device v4l2_dev;
	struct v4l2_async_notifier notifier;

	/* Guards @sensor against unbind racing a write to the stream attr. */
	struct mutex lock;
	struct v4l2_subdev *sensor;
	bool streaming;
};

static int duos_cam_stub_bound(struct v4l2_async_notifier *notifier,
			       struct v4l2_subdev *sd,
			       struct v4l2_async_connection *asc)
{
	struct duos_cam_stub *stub =
		container_of(notifier, struct duos_cam_stub, notifier);

	guard(mutex)(&stub->lock);
	stub->sensor = sd;

	dev_info(stub->v4l2_dev.dev, "bound sensor %s\n", sd->name);

	return 0;
}

static void duos_cam_stub_unbind(struct v4l2_async_notifier *notifier,
				 struct v4l2_subdev *sd,
				 struct v4l2_async_connection *asc)
{
	struct duos_cam_stub *stub =
		container_of(notifier, struct duos_cam_stub, notifier);

	guard(mutex)(&stub->lock);

	/*
	 * The sensor is going away underneath us. Its own remove() stops
	 * streaming, so do not call into it here - just forget it.
	 */
	stub->sensor = NULL;
	stub->streaming = false;
}

static int duos_cam_stub_complete(struct v4l2_async_notifier *notifier)
{
	struct duos_cam_stub *stub =
		container_of(notifier, struct duos_cam_stub, notifier);

	/* This is the whole point of the module. */
	return v4l2_device_register_subdev_nodes(&stub->v4l2_dev);
}

static const struct v4l2_async_notifier_operations duos_cam_stub_notifier_ops = {
	.bound = duos_cam_stub_bound,
	.unbind = duos_cam_stub_unbind,
	.complete = duos_cam_stub_complete,
};

static ssize_t sensor_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct duos_cam_stub *stub = dev_get_drvdata(dev);

	guard(mutex)(&stub->lock);

	return sysfs_emit(buf, "%s\n", stub->sensor ? stub->sensor->name : "");
}
static DEVICE_ATTR_RO(sensor);

static ssize_t stream_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct duos_cam_stub *stub = dev_get_drvdata(dev);

	guard(mutex)(&stub->lock);

	return sysfs_emit(buf, "%d\n", stub->streaming);
}

static ssize_t stream_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t len)
{
	struct duos_cam_stub *stub = dev_get_drvdata(dev);
	bool enable;
	int ret;

	ret = kstrtobool(buf, &enable);
	if (ret)
		return ret;

	guard(mutex)(&stub->lock);

	if (!stub->sensor)
		return -ENODEV;

	if (enable == stub->streaming)
		return len;

	ret = v4l2_subdev_call(stub->sensor, video, s_stream, enable);
	if (ret)
		return ret;

	stub->streaming = enable;

	return len;
}
static DEVICE_ATTR_RW(stream);

static struct attribute *duos_cam_stub_attrs[] = {
	&dev_attr_sensor.attr,
	&dev_attr_stream.attr,
	NULL,
};
ATTRIBUTE_GROUPS(duos_cam_stub);

static int duos_cam_stub_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct v4l2_async_connection *asc;
	struct duos_cam_stub *stub;
	struct fwnode_handle *ep;
	int ret;

	stub = devm_kzalloc(dev, sizeof(*stub), GFP_KERNEL);
	if (!stub)
		return -ENOMEM;

	ret = devm_mutex_init(dev, &stub->lock);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, stub);

	/*
	 * Nothing here can be devm-managed: the notifier and the v4l2_device
	 * have to come down in a defined order relative to each other, and
	 * before the struct they live in is freed.
	 */
	ret = v4l2_device_register(dev, &stub->v4l2_dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register v4l2 device\n");

	/*
	 * Same lookup the sensor does from the other end. It cannot fail for
	 * lack of a remote here - the sensor node is what the remote points at,
	 * and a DT node with no status property counts as available whether or
	 * not its driver is loaded.
	 */
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(dev), 0, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		ret = dev_err_probe(dev, -EINVAL, "missing endpoint node\n");
		goto err_v4l2_unregister;
	}

	v4l2_async_nf_init(&stub->notifier, &stub->v4l2_dev);
	stub->notifier.ops = &duos_cam_stub_notifier_ops;

	asc = v4l2_async_nf_add_fwnode_remote(&stub->notifier, ep,
					      struct v4l2_async_connection);
	fwnode_handle_put(ep);
	if (IS_ERR(asc)) {
		ret = dev_err_probe(dev, PTR_ERR(asc),
				    "failed to add async connection\n");
		goto err_nf_cleanup;
	}

	ret = v4l2_async_nf_register(&stub->notifier);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register notifier\n");
		goto err_nf_cleanup;
	}

	return 0;

err_nf_cleanup:
	v4l2_async_nf_cleanup(&stub->notifier);
err_v4l2_unregister:
	v4l2_device_unregister(&stub->v4l2_dev);

	return ret;
}

static void duos_cam_stub_remove(struct platform_device *pdev)
{
	struct duos_cam_stub *stub = platform_get_drvdata(pdev);

	scoped_guard(mutex, &stub->lock) {
		if (stub->streaming && stub->sensor)
			v4l2_subdev_call(stub->sensor, video, s_stream, 0);
		stub->streaming = false;
	}

	v4l2_async_nf_unregister(&stub->notifier);
	v4l2_async_nf_cleanup(&stub->notifier);
	v4l2_device_unregister(&stub->v4l2_dev);
}

static const struct of_device_id duos_cam_stub_of_match[] = {
	{ .compatible = "milkv,duos-cam-stub" },
	{ }
};
MODULE_DEVICE_TABLE(of, duos_cam_stub_of_match);

static struct platform_driver duos_cam_stub_driver = {
	.driver = {
		.name = "duos-cam-stub",
		.of_match_table = duos_cam_stub_of_match,
		.dev_groups = duos_cam_stub_groups,
	},
	.probe = duos_cam_stub_probe,
	.remove = duos_cam_stub_remove,
};
module_platform_driver(duos_cam_stub_driver);

MODULE_DESCRIPTION("Stub V4L2 receiver for Milk-V Duo S camera bring-up");
MODULE_LICENSE("GPL");
