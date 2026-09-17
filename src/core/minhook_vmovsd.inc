#if defined(_M_X64) || defined(__x86_64__)
        {
            const unsigned char* vex = (const unsigned char*)pOldInst;
            if (oldPos < sizeof(JMP_REL) && vex[0] == 0xC5 && vex[1] == 0xFB
                && vex[2] == 0x10 && (vex[3] & 0xC0) == 0x40
                && (vex[3] & 7) != 4)
            {
                memset(&hs, 0, sizeof(hs));
                hs.len = 5;
                hs.opcode = 0x10;
                hs.modrm = vex[3];
                hs.modrm_mod = 1;
                hs.modrm_reg = (vex[3] >> 3) & 7;
                hs.modrm_rm = vex[3] & 7;
                hs.flags = F_MODRM | F_DISP8;
                hs.disp.disp8 = vex[4];
                copySize = 5;
            }
        }
#endif
