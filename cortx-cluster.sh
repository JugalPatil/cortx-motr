#!/bin/sh

set -e

start_time=`date +%s`

echo "##################################"
echo "########### BUILD MOTR ###########"
echo "##################################"

MOTR_DIR="/root/cortx-motr"

echo "########### Cloning Motr ###########"
git clone --recursive https://github.com/Seagate/cortx-motr.git -b main $MOTR_DIR

echo "Install Motr Build Deps"
$MOTR_DIR/scripts/install-build-deps

echo "########### Make Motr RPMS ###########"
cd $MOTR_DIR && ./autogen.sh && ./configure && make rpms

echo "########### Install RPMS ###########"
rpm -ivh /root/rpmbuild/RPMS/x86_64/cortx-motr-2.0.0-*
rpm -ivh /root/rpmbuild/RPMS/x86_64/cortx-motr-devel-2.0.0-*

echo "##################################"
echo "########### BUILD UTILS ###########"
echo "##################################"

UTILS_DIR="/root/cortx-utils"

echo "########### Cloning Utils ###########"
git clone --recursive https://github.com/Seagate/cortx-utils -b main $UTILS_DIR

echo "########### Install Utils Build Deps ###########"
yum install -y gcc rpm-build python36 python36-devel openssl-devel libffi-devel

echo "########### Make Utils RPMS ###########"
cd $UTILS_DIR && ./jenkins/build.sh -v 2.0.0 -b 2

echo "########### Install Utils Prerequisites ###########"
pip3 install -r https://raw.githubusercontent.com/Seagate/cortx-utils/main/py-utils/python_requirements.txt
pip3 install -r https://raw.githubusercontent.com/Seagate/cortx-utils/main/py-utils/python_requirements.ext.txt

echo "########### Install Utils RPM ###########"
rpm -ivh $UTILS_DIR/py-utils/dist/cortx-py-utils-*.noarch.rpm --nodeps

echo "##################################"
echo "########### BUILD HARE ###########"
echo "##################################"

HARE_DIR="/root/cortx-hare"
echo "########### Cloning Hare ###########"
git clone https://github.com/Seagate/cortx-hare.git -b main $HARE_DIR

echo "########### Install Hare Build deps ###########"
yum -y install python3 python3-devel facter
yum -y install yum-utils
yum-config-manager --add-repo https://rpm.releases.hashicorp.com/RHEL/hashicorp.repo
yum -y install consul-1.9.1

echo "########### Make Hare RPMS ###########"
cd $HARE_DIR && make rpm
rpm -ivh /root/rpmbuild/RPMS/x86_64/cortx-hare-2.0.0-*

echo "########### Create CDF.yaml ###########"
cat <<EOF | sudo tee ~/CDF.yaml
# Cluster Description File (CDF).
# See `cfgen --help-schema` for the format description.

nodes:
  - hostname: localhost     # [user@]hostname
    data_iface: eth1        # name of data network interface
    #data_iface_type: o2ib  # type of network interface (optional);
                            # supported values: "tcp" (default), "o2ib"
    transport_type: libfab
    m0_servers:
      - runs_confd: true
        io_disks:
          data: []
      - io_disks:
          #meta_data: /path/to/meta-data/drive
          data:
            - path: /dev/sdc
            - path: /dev/sdd
    m0_clients:
      s3: 0         # number of S3 servers to start
      other: 2      # max quantity of other Motr clients this host may have
create_aux: false # optional; supported values: "false" (default), "true"
pools:
  - name: the pool
    type: sns  # optional; supported values: "sns" (default), "dix", "md"
    disk_refs:
      - { path: /dev/sdc, node: localhost }
      - { path: /dev/sdd, node: localhost }
    data_units: 1
    parity_units: 0
    spare_units: 0
    #allowed_failures: { site: 0, rack: 0, encl: 0, ctrl: 0, disk: 0 }
#profiles:
#  - name: default
#    pools: [ the pool ]
#fdmi_filters:
#  - name: test
#    node: localhost
#    client_index: 1
#    substrings: ["Bucket-Name", "Object-Name", "Object-URI"]
EOF

echo "########### Start Cluster ###########"
hctl bootstrap --mkfs ~/CDF.yaml

end_time=`date +%s`
echo execution time was `expr $end_time - $start_time` s.