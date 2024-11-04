#!/bin/bash

PASSLESS_ENTRY="/home/domin/.ssh/id_rsa"

ssh -o StrictHostKeyChecking=no -i $PASSLESS_ENTRY domin@192.168.60.172 "cd /home/domin/Embarcadero/build/bin && ./scalog_global_sequencer"
