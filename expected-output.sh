#!/bin/sh
cat << EOF
slpm `git describe --long`
SLPM_FULLNAME='John Doe'
Passphrase: Deriving key...Key derivation complete.
Site: Counter: Maximum Security Password: F!FpQM4RIUyXSKOFue9%
Long Password: QiceYupp6,Himd
Medium Password: QicXey0.
Short Password: Qic6
Basic Password: FB56BMr5
PIN: 7256
Site: Counter: Maximum Security Password: JPtM*En%Gh%1a^n8OU2.
Long Password: HigvJiwa4*Qasg
Medium Password: HigVuk5~
Short Password: Hig3
Basic Password: JY63KuY0
PIN: 8763
Site: Counter: ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIIuUymVj8nCcFfItW98tJNGMiLKV0dKso8UUFw6VME73 jdoe@slpm+github.com
Site: Bye!    
EOF
