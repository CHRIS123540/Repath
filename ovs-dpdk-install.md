# DPU 安装 ovs-dpdk

## 1、查找对应版本的dpdk和ovs

- [ovs与linux kernel、dpdk版本对应关系](https://docs.openvswitch.org/en/latest/faq/releases/)

DPU上原本的ovs和dpdk版本如下：

```textile
root@localhost:~# ovs-vswitchd --version
ovs-vswitchd (Open vSwitch) 2.17.2-24a81c8
MLNX_DPDK 20.11.6.1.5
```

> **!!!** **选择 ovs 版本为 2.17.2 ，但是经尝试后发现DPU自带的MLNX_DPDK 20.11.6.1.5 无法编译 ovs，因此选择编译新版本的dpdk-21.11.6**

## 2、编译dpdk

- [dpdk源码下载地址 ](http://fast.dpdk.org/rel/)

- 此处使用 [dpdk-21.11.6](http://fast.dpdk.org/rel/dpdk-21.11.6.tar.gz)

```bash
# 下载源码，也可以上传
wget http://fast.dpdk.org/rel/dpdk-21.11.6.tar.gz
tar -zxvf dpdk-21.11.6.tar.xz
cd dpdk-stable-21.11.6
```

- **问题：** DPU (BF-2) 上安装dpdk有一个驱动问题，解决方式如下：

```bash
vim dpdk-stable-21.11.6/drivers/regex/octeontx2/meson.build
```

将 `lib = cc.find_library('rxp_compiler', required: false)`  

改为 `lib = cc.find_library('rxpcompiler', required: false)` 

- 编译安装

```bash
export DPDK_DIR=$PWD
export DPDK_BUILD=$DPDK_DIR/build
meson build
ninja -C build
ninja -C build install
ldconfig
```

> **安装好的版本可能和默认版本冲突，修改方式如下：**

- 使用 DPDK-21.11.6：

```bash
mv /opt/mellanox/dpdk /opt/mellanox/dpdk_bak
ldconfig
```

- 使用默认版本dpdk：

```bash
cd dpdk-statble-21.11/build
ninja uninstall
mv /opt/mellanox/dpdk_bak /opt/mellanox/dpdk
export PKG_CONFIG_PATH=$PKG_CONFIG_PATH:/opt/mellanox/dpdk/lib/aarch64-linux-gnu/pkgconfig
ldconfig
```

- 查看当前dpdk版本：

```bash
pkg-config --modversion libdpdk
```

## 2 编译ovs-dpdk源码

- [ovs源码下载地址](https://www.openvswitch.org/download/)

- 此处使用 [openvswitch-2.17.2](https://www.openvswitch.org/releases/openvswitch-2.17.2.tar.gz)

- 编译安装ovs-dpdk

```bash
# 自定义安装路径
mkdir -p ovs/etc ovs/usr ovs/var
# 下载源码，也可以上传
wget https://www.openvswitch.org/releases/openvswitch-2.17.2.tar.gz
tar -zxvf openvswitch-2.17.2.tar.gz
cd openvswitch-2.17.2
# 配置自定义目录及相关参数， /path/修改为自己的ovs路径
./configure \
--prefix=/path/ovs/usr \
--localstatedir=/path/ovs/var \
--sysconfdir=/path/ovs/etc --with-dpdk=static
# 编译
make
# 安装，安装在configure配置的目录中，后续运行的命令也在这些目录中
make install
```

- 查看大页，记得分配

```bash
grep HugePages_ /proc/meminfo
```

- 设置并开启ovs-dpdk

```bash
ovs/usr/share/openvswitch/scripts/ovs-ctl --system-id=random start
ovs/usr/bin/ovs-vsctl --no-wait set Open_vSwitch . other_config:dpdk-init=true
ovs/usr/bin/ovs-vsctl --no-wait set Open_vSwitch . other_config:dpdk-socket-mem=1024
```

- 查询版本及配置

```bash
ovs/usr/sbin/ovs-vswitchd --version
ovs/usr/bin/ovs-vsctl get Open_vSwitch . dpdk_version
# 确认是否初始化ovs-dpdk
ovs/usr/bin/ovs-vsctl get Open_vSwitch . dpdk_initialized
```

## 3 ovs-dpdk和原ovs冲突问题

- 新的ovs-dpdk一旦网桥配置后，原先的ovs就会失效，可能是数据库没有配置正确

- 还原方式

```bash
# 关闭新ovs-dpdk的vswitchd
ovs/usr/share/openvswitch/scripts/ovs-ctl stop
# 重启原先的ovs
/etc/init.d/openvswitch-switch restart
```

- 关闭原先的ovs，使用ovs-dpdk

```bash
# 停止原先的ovs服务
/etc/init.d/openvswitch-switch stop
# 开启ovs-dpdk
ovs/usr/share/openvswitch/scripts/ovs-ctl --system-id=random start
```

- **重要：ovs-dpdk启动和配置的命令和方式可能有误，有新的或者正确的方式请添加或修改以上内容**
