# P4交换机下mac流表并在DPU上配置VxLAN

引言： `ovs-dpdk` 方案在 `DPU` 上部署 `vxlan` ，需要使用 `p0` 作为 `underlay` ， `overlay` 是上层 `HOST` 的 `PF` 及 `VF` ，需要给 `switch2` 和 `switch3` 配置指定的 `p0` 口 `mac`

## 1 修改P4交换机上的脚本

- 文件位置： `/root/cq/ecmpudp/HOPA/benchmark.py`
  
- `Switch2` 上在以下位置添加: `"CQ-DPU-p0"` 和 `"YY-DPU-p0"` 两行， `mac` 地址是各自 `p0` 口的 `mac`
  

```python
def runTest(self): #this switch is s2
    ThisSwitchPort = [{"Name": "CQ-Host", "port": 4,"dst_mac": '08:c0:eb:bf:ef:9a'}, 
            {"Name": "CQ-DPU", "port": 4,"dst_mac": '02:1e:a9:bf:27:47'},
            {"Name": "CQ-DPU-p0", "port": 4,"dst_mac": '08:c0:eb:bf:ef:9e'},
            {"Name": "Node2-p1", "port": 140,"dst_mac": 'b8:ce:f6:d5:d6:f7'}]

    RemoteSwitchPort = [{"Name": "YY-Host", "port": 56,"dst_mac": '08:c0:eb:bf:ef:82'}, 
            {"Name": "YY-DPU", "port": 56,"dst_mac": '02:b1:04:57:76:77'},
            {"Name": "YY-DPU-p0", "port": 56,"dst_mac": '08:c0:eb:bf:ef:86'},
            {"Name": "Node3-p1", "port": 56,"dst_mac": 'b8:ce:f6:d5:cc:5f'}]
```

- `Switch3` 上在以下位置添加: `"YY-DPU-p0"` 和 `"CQ-DPU-p0"` 两行， `mac` 地址是各自 `p0` 口的 `mac`

```python
def runTest(self):  # this switch is s3
    ThisSwitchPort = [{"Name": "YY-Host", "port": 4,"dst_mac": '08:c0:eb:bf:ef:82'}, 
            {"Name": "YY-DPU", "port": 4,"dst_mac": '02:b1:04:57:76:77'},
            {"Name": "YY-DPU-p0", "port": 4,"dst_mac": '08:c0:eb:bf:ef:86'},
            {"Name": "Node3-p1", "port": 140,"dst_mac": 'b8:ce:f6:d5:cc:5f'},
            {"Name":"Node3-40GbE-p1","port":132,"dst_mac":"3c:fd:fe:9e:86:ca"}]

    RemoteSwitchPort = [{"Name": "CQ-Host", "port": 56,"dst_mac": '08:c0:eb:bf:ef:9a'},
            {"Name": "CQ-DPU", "port": 56,"dst_mac": '02:1e:a9:bf:27:47'},
            {"Name": "CQ-DPU-p0", "port": 56,"dst_mac": '08:c0:eb:bf:ef:9e'},
            {"Name": "Node2-p1", "port": 56,"dst_mac": 'b8:ce:f6:d5:d6:f7'}]
```

- 重新编译一下这两个文件
- 配置vxlan拓扑在另一篇

## 2 在arm上添加相应的arp

```bash
# CQ-DPU
arp -s 192.168.240.1 08:c0:eb:bf:ef:86
# YY-DPU
arp -s 192.168.240.2 08:c0:eb:bf:ef:9e
# 查看arp缓存内容
arp -a
```

## 3 给br-phy添加相应的arp

```bash
# CQ-DPU
./ovs-appctl tnl/arp/set br-phy 192.168.240.1 08:c0:eb:bf:ef:86
# YY-DPU
./ovs-appctl tnl/arp/set br-phy 192.168.240.2 08:c0:eb:bf:ef:9e
# 查看arp缓存内容
./ovs-appctl tnl/arp/show
```