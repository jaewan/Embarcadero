fio --name=write-test --rw=write --bs=1k --size=10G --numjobs=1 --direct=1 --filename=test_file.img
fio --name=write-test --rw=write --bs=1k --size=10G --numjobs=2 --direct=2 --filename=test_file.img
fio --name=write-test --rw=write --bs=1k --size=10G --numjobs=4 --direct=4 --filename=test_file.img
