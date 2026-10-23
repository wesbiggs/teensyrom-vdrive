#ifdef Fab04_VDrive
FLASHMEM void VDriveRemoteCommand(bool Set)
{
   //   App: VDriveGetToken 0x64E3, Drive(1)                        Drive: as the file commands, USBDrive = 0, SD = 1
   //   App: VDriveSetToken 0x64E4, Drive(1), ImagePath (null term)  ImagePath: "/dir/IMAGE.D64" on that unit's card, "" = unmount
   //Teensy: AckToken 0x64CC and one line of JSON (the unit's state after the change), or
   //        FailToken 0x9B7F and a line of text with the reason
   //   Works while the VDrive handler runs a C64 program (the handler-busy check lets it through).  With the TR menu showing (VDrive not
   //   running) the same commands report / rewrite the unit's remembered image (/.vdrive), which is what it mounts at the next start;
   //   the JSON then has "running":false.  While it runs, a change of disk waits for a quiet moment on the C64's drive (no LOAD/SAVE in
   //   flight; up to 3 s) and closes the unit's open channels.
   uint32_t Drive;
   if (!GetUInt(&Drive, 1)) return;
   char Path[MaxNamePathLength];
   Path[0] = 0;
   if (Set)
   {
      uint16_t CharNum = 0;
      while (1)
      {
         if (!SerialAvailabeTimeout()) return;
         Path[CharNum] = CmdChannel->read();
         if (Path[CharNum] == 0) break;
         if (++CharNum == MaxNamePathLength)
         {
            SendU16(FailToken);
            CmdChannel->print("Path too long!\n");
            return;
         }
      }
   }
   if (Drive != rmtSD && Drive != rmtUSBDrive)
   {
      SendU16(FailToken);
      CmdChannel->print("Drive must be USB (0) or SD (1)\n");
      return;
   }
   int Unit = (Drive == rmtSD) ? 0 : 1;   //VDrive's own numbering: 0 = SD, 1 = USB
   bool Running = VD_IsHandler(CurrentIOHandler);
   char Msg[64];
   char Json[2 * MaxNamePathLength + 160];
   if (!Running)
   {  //TR menu showing: report / change what the unit will mount at the next start (its /.vdrive sidecar)
      if (Set && VDrive_RemoteIdleSet(Unit, Path, Msg, sizeof Msg) != 0)
      {
         SendU16(FailToken);
         CmdChannel->printf("%s\n", Msg);
         return;
      }
      if (VDrive_RemoteIdleStatus(Unit, Json, sizeof Json) == 0)
      {
         SendU16(FailToken);
         CmdChannel->print("No such drive\n");
         return;
      }
      SendU16(AckToken);
      CmdChannel->printf("%s\n", Json);
      return;
   }
   if (Set && VDrive_RemoteSet(Unit, Path, Msg, sizeof Msg) != 0)
   {
      SendU16(FailToken);
      CmdChannel->printf("%s\n", Msg);
      return;
   }
   if (VDrive_RemoteStatus(Unit, Json, sizeof Json) == 0)
   {
      SendU16(FailToken);
      CmdChannel->print("No such drive\n");
      return;
   }
   SendU16(AckToken);
   CmdChannel->printf("%s\n", Json);
}
#endif
