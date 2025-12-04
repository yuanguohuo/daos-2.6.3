#!/bin/bash

function ensure_attach_info_file()
{
	local info_file=/tmp/IV_TEST.attach_info_tmp
	if [ -d $info_file ] ; then
		echo "ERROR: $info_file cannot be a directory"
		return 1
	fi
	if [ -S $info_file ] ; then
		echo "ERROR: $info_file cannot be a socket"
		return 1
	fi
	if [ -b $info_file ] ; then
		echo "ERROR: $info_file cannot be a block special file"
		return 1
	fi

	cat << EOF > $info_file
name IV_TEST
size 2
all
0 ofi+tcp://101.67.23.169:8686
1 ofi+tcp://101.67.23.170:8686
EOF
}

#if a IV_SERVER has been started on current machine, "/tmp/IV_TEST.attach_info_tmp" has been created by it.
#otherwise, we must create it by ourself;
ensure_attach_info_file && cat /tmp/IV_TEST.attach_info_tmp

#update key of rank0 by rank0
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o update -r 0 -k 0:0 -v AAAA

#update key of rank0 by rank1
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o update -r 1 -k 0:1 -v BBBB

#fetch key of rank0 by rank0
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o fetch -r 0 -k 0:0
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o fetch -r 0 -k 0:1

#fetch key of rank0 by rank1
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o fetch -r 1 -k 0:0
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o fetch -r 1 -k 0:1


#if current client is on the same machine of IV_SERVER, the "/tmp/IV_TEST.attach_info_tmp" file will be removed when IV_SERVER is shutdown,
#and we cannot run subsequent commands; so we must ensure the file exists (ensure_attach_info_file)

ensure_attach_info_file
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o shutdown -r 0
ensure_attach_info_file
D_PROVIDER="ofi+tcp" D_INTERFACE=bond0 /usr/lib/daos/TESTING/tests/iv_client -o shutdown -r 1
