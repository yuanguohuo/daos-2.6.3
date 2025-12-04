#!/bin/bash

MY_RANK=0

cat << EOF > /tmp/crt_verbs_grp.cfg
0 ofi+verbs;ofi_rxm://101.67.23.169:8686
1 ofi+verbs;ofi_rxm://101.67.23.170:8686
EOF

CRT_L_RANK=${MY_RANK} D_PROVIDER="ofi+verbs;ofi_rxm" D_INTERFACE=bond0 CRT_L_GRP_CFG=/tmp/crt_verbs_grp.cfg D_PORT=8686 /usr/lib/daos/TESTING/tests/iv_server
