/*++

Copyright (c) 1989-2000 Microsoft Corporation

Module Name:

    Cleanup.c

Abstract:

    This module implements the File Cleanup routine for Fat called by the
    dispatch driver.


--*/

#include "FatProcs.h"

//
//  The Bug check file id for this module
//

#define BugCheckFileId                   (FAT_BUG_CHECK_CLEANUP)

//
//  The local debug trace level
//

#define Dbg                              (DEBUG_TRACE_CLEANUP)

//
//  The following little routine exists solely because it need a spin lock.
//

VOID
FatAutoUnlock (
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb
    );

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, FatCommonCleanup)
#pragma alloc_text(PAGE, FatFsdCleanup)
#endif


NTSTATUS
FatFsdCleanup (
    IN PVOLUME_DEVICE_OBJECT VolumeDeviceObject,
    IN PIRP Irp
    )

/*++

Routine Description:

    This routine implements the FSD part of closing down a handle to a
    file object.

Arguments:

    VolumeDeviceObject - Supplies the volume device object where the
        file being Cleanup exists

    Irp - Supplies the Irp being processed

Return Value:

    NTSTATUS - The FSD status for the IRP

--*/

{
    NTSTATUS Status;
    PIRP_CONTEXT IrpContext = NULL;

    BOOLEAN TopLevel;

    PAGED_CODE();

    //
    //  If we were called with our file system device object instead of a
    //  volume device object, just complete this request with STATUS_SUCCESS
    //

    if ( FatDeviceIsFatFsdo( VolumeDeviceObject))  {

        Irp->IoStatus.Status = STATUS_SUCCESS;
        Irp->IoStatus.Information = FILE_OPENED;

        IoCompleteRequest( Irp, IO_DISK_INCREMENT );

        return STATUS_SUCCESS;
    }

    DebugTrace(+1, Dbg, "FatFsdCleanup\n", 0);

    //
    //  Call the common Cleanup routine, with blocking allowed.
    //

    FsRtlEnterFileSystem();

    TopLevel = FatIsIrpTopLevel( Irp );

    __try {

        IrpContext = FatCreateIrpContext( Irp, TRUE );

        Status = FatCommonCleanup( IrpContext, Irp );

    } __except(FatExceptionFilter( IrpContext, GetExceptionInformation() )) {

        //
        //  We had some trouble trying to perform the requested
        //  operation, so we'll abort the I/O request with
        //  the error status that we get back from the
        //  execption code
        //

        Status = FatProcessException( IrpContext, Irp, GetExceptionCode() );
    }

    if (TopLevel) { IoSetTopLevelIrp( NULL ); }

    FsRtlExitFileSystem();

    //
    //  And return to our caller
    //

    DebugTrace(-1, Dbg, "FatFsdCleanup -> %08lx\n", Status);

    UNREFERENCED_PARAMETER( VolumeDeviceObject );

    return Status;
}


__drv_mustHoldCriticalRegion    
NTSTATUS
FatCommonCleanup (
    IN PIRP_CONTEXT IrpContext,
    IN PIRP Irp
    )

/*++

Routine Description:

    This is the common routine for cleanup of a file/directory called by both
    the fsd and fsp threads.

    Cleanup is invoked whenever the last handle to a file object is closed.
    This is different than the Close operation which is invoked when the last
    reference to a file object is deleted.

    The function of cleanup is to essentially "cleanup" the file/directory
    after a user is done with it.  The Fcb/Dcb remains around (because MM
    still has the file object referenced) but is now available for another
    user to open (i.e., as far as the user is concerned the is now closed).

    See close for a more complete description of what close does.

Arguments:

    Irp - Supplies the Irp to process

Return Value:

    NTSTATUS - The return status for the operation

--*/

{
    NTSTATUS Status = STATUS_SUCCESS;

    PIO_STACK_LOCATION IrpSp;

    PFILE_OBJECT FileObject;

    TYPE_OF_OPEN TypeOfOpen;
    PVCB Vcb;
    PFCB Fcb;
    PCCB Ccb;

    BOOLEAN SendUnlockNotification = FALSE;

    PSHARE_ACCESS ShareAccess;

    PLARGE_INTEGER TruncateSize = NULL;
    LARGE_INTEGER LocalTruncateSize;

    BOOLEAN AcquiredVcb = FALSE;
    BOOLEAN AcquiredFcb = FALSE;

    PAGED_CODE();

    IrpSp = IoGetCurrentIrpStackLocation( Irp );

    DebugTrace(+1, Dbg, "FatCommonCleanup\n", 0);
    DebugTrace( 0, Dbg, "Irp           = %08lx\n", Irp);
    DebugTrace( 0, Dbg, "->FileObject  = %08lx\n", IrpSp->FileObject);

    //
    //  Extract and decode the file object
    //

    FileObject = IrpSp->FileObject;
    TypeOfOpen = FatDecodeFileObject( FileObject, &Vcb, &Fcb, &Ccb );

    //
    //  Special case the unopened file object.  This will occur only when
    //  we are initializing Vcb and IoCreateStreamFileObject is being
    //  called.
    //

    if (TypeOfOpen == UnopenedFileObject) {

        DebugTrace(0, Dbg, "Unopened File Object\n", 0);

        FatCompleteRequest( IrpContext, Irp, STATUS_SUCCESS );

        DebugTrace(-1, Dbg, "FatCommonCleanup -> STATUS_SUCCESS\n", 0);
        return STATUS_SUCCESS;
    }

    //
    //  If this is not our first time through (for whatever reason)
    //  only see if we have to flush the file.
    //

    if (FlagOn( FileObject->Flags, FO_CLEANUP_COMPLETE )) {

        if (FlagOn(Vcb->VcbState, VCB_STATE_FLAG_DEFERRED_FLUSH) &&
            FlagOn(FileObject->Flags, FO_FILE_MODIFIED) &&
            !FlagOn(Vcb->VcbState, VCB_STATE_FLAG_WRITE_PROTECTED) &&
            (TypeOfOpen == UserFileOpen)) {

            //
            //  Flush the file.
            //

            Status = FatFlushFile( IrpContext, Fcb, Flush );

            if (!NT_SUCCESS(Status)) {

                FatNormalizeAndRaiseStatus( IrpContext, Status );
            }
        }

        FatCompleteRequest( IrpContext, Irp, STATUS_SUCCESS );

        DebugTrace(-1, Dbg, "FatCommonCleanup -> STATUS_SUCCESS\n", 0);
        return STATUS_SUCCESS;
    }

    //
    //  If we call change the allocation or call CcUninitialize,
    //  we have to take the Fcb exclusive
    //

    if ((TypeOfOpen == UserFileOpen) || (TypeOfOpen == UserDirectoryOpen)) {

        ASSERT( Fcb != NULL );
    
        (VOID)FatAcquireExclusiveFcb( IrpContext, Fcb );

        AcquiredFcb = TRUE;

        //
        //  Do a check here if this was a DELETE_ON_CLOSE FileObject, and
        //  set the Fcb flag appropriately.
        //

        if (FlagOn(Ccb->Flags, CCB_FLAG_DELETE_ON_CLOSE)) {

            ASSERT( NodeType(Fcb) != FAT_NTC_ROOT_DCB );

            SetFlag(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE);

            //
            //  Report this to the dir notify package for a directory.
            //

            if (TypeOfOpen == UserDirectoryOpen) {

#pragma prefast( suppress:6309, "FullDirectoryName may be NULL if NotifyIrp is also NULL. this indicates the object is being deleted." )
                FsRtlNotifyFullChangeDirectory( Vcb->NotifySync,
                                                &Vcb->DirNotifyList,
                                                FileObject->FsContext,
                                                NULL,
                                                FALSE,
                                                FALSE,
                                                0,
                                                NULL,
                                                NULL,
                                                NULL );
            }
        }

        //
        //  Now if we may delete the file, drop the Fcb and acquire the Vcb
        //  first.  Note that while we own the Fcb exclusive, a file cannot
        //  become DELETE_ON_CLOSE and cannot be opened via CommonCreate.
        //

        if ((Fcb->UncleanCount == 1) &&
            FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE) &&
            (Fcb->FcbCondition != FcbBad) &&
            !FlagOn(Vcb->VcbState, VCB_STATE_FLAG_WRITE_PROTECTED)) {

            FatReleaseFcb( IrpContext, Fcb );
            AcquiredFcb = FALSE;

            (VOID)FatAcquireExclusiveVcb( IrpContext, Vcb );
            AcquiredVcb = TRUE;

            (VOID)FatAcquireExclusiveFcb( IrpContext, Fcb );
            AcquiredFcb = TRUE;
        }
    }

    //
    //  For user DASD cleanups, grab the Vcb exclusive.
    //

    if (TypeOfOpen == UserVolumeOpen) {

        (VOID)FatAcquireExclusiveVcb( IrpContext, Vcb );
        AcquiredVcb = TRUE;
    }

    //
    //  Complete any Notify Irps on this file handle.
    //

    if (TypeOfOpen == UserDirectoryOpen) {

        FsRtlNotifyCleanup( Vcb->NotifySync,
                            &Vcb->DirNotifyList,
                            Ccb );
    }

    //
    //  Determine the Fcb state, Good or Bad, for better or for worse.
    //
    //  We can only read the volume file if VcbCondition is good.
    //

    if ( Fcb != NULL) {

        //
        //  Stop any raises from FatVerifyFcb, unless it is REAL bad.
        //

        __try {

            __try {

                FatVerifyFcb( IrpContext, Fcb );

            } __except( FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                      EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) {

                  FatResetExceptionState( IrpContext );
            }

        } __finally {

            if ( AbnormalTermination() ) {

                //
                //  We will be raising out of here.
                //

                if (AcquiredFcb) { FatReleaseFcb( IrpContext, Fcb ); }
                if (AcquiredVcb) { FatReleaseVcb( IrpContext, Vcb ); }
            }
        }
    }

    __try {

        //
        //  Case on the type of open that we are trying to cleanup.
        //  For all cases we need to set the share access to point to the
        //  share access variable (if there is one). After the switch
        //  we then remove the share access and complete the Irp.
        //  In the case of UserFileOpen we actually have a lot more work
        //  to do and we have the FsdLockControl complete the Irp for us.
        //

        switch (TypeOfOpen) {

        case DirectoryFile:
        case VirtualVolumeFile:

            DebugTrace(0, Dbg, "Cleanup VirtualVolumeFile/DirectoryFile\n", 0);

            ShareAccess = NULL;

            break;

        case UserVolumeOpen:

            DebugTrace(0, Dbg, "Cleanup UserVolumeOpen\n", 0);

            if (FlagOn( Ccb->Flags, CCB_FLAG_COMPLETE_DISMOUNT )) {

                FatCheckForDismount( IrpContext, Vcb, TRUE );
            
            //
            //  If this handle had write access, and actually wrote something,
            //  flush the device buffers, and then set the verify bit now
            //  just to be safe (in case there is no dismount).
            //

            } else if (FileObject->WriteAccess &&
                       FlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {

                (VOID)FatHijackIrpAndFlushDevice( IrpContext,
                                                  Irp,
                                                  Vcb->TargetDeviceObject );

                SetFlag(Vcb->Vpb->RealDevice->Flags, DO_VERIFY_VOLUME);
            }

            //
            //  If the volume is locked by this file object then release
            //  the volume and send notification.
            //

            if (FlagOn(Vcb->VcbState, VCB_STATE_FLAG_LOCKED) &&
                (Vcb->FileObjectWithVcbLocked == FileObject)) {

                FatAutoUnlock( IrpContext, Vcb );
                SendUnlockNotification = TRUE;
            }

            ShareAccess = &Vcb->ShareAccess;

            break;

        case EaFile:

            DebugTrace(0, Dbg, "Cleanup EaFileObject\n", 0);

            ShareAccess = NULL;

            break;

        case UserDirectoryOpen:

            DebugTrace(0, Dbg, "Cleanup UserDirectoryOpen\n", 0);

            ShareAccess = &Fcb->ShareAccess;

            //
            //  Determine here if we should __try do delayed close.
            //

            if ((Fcb->UncleanCount == 1) &&
                (Fcb->OpenCount == 1) &&
                (Fcb->Specific.Dcb.DirectoryFileOpenCount == 0) &&
                !FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE) &&
                Fcb->FcbCondition == FcbGood) {

                //
                //  Delay our close.
                //

                SetFlag( Fcb->FcbState, FCB_STATE_DELAY_CLOSE );
            }

            //
            //  Clear the deny defrag bit, if the handle we're cleaning up was the one that set it.
            //
            
            if( FlagOn(Fcb->FcbState, FCB_STATE_DENY_DEFRAG) && FlagOn(Ccb->Flags, CCB_FLAG_DENY_DEFRAG) ) {
                
                ClearFlag(Ccb->Flags, CCB_FLAG_DENY_DEFRAG);
                ClearFlag(Fcb->FcbState, FCB_STATE_DENY_DEFRAG );
            }

            if (VcbGood == Vcb->VcbCondition) {
            
                FatUpdateDirentFromFcb( IrpContext, FileObject, Fcb, Ccb );
            }
            
            //
            //  If the directory has a unclean count of 1 then we know
            //  that this is the last handle for the file object.  If
            //  we are supposed to delete it, do so.
            //

            if ((Fcb->UncleanCount == 1) &&
                (NodeType(Fcb) == FAT_NTC_DCB) &&
                (FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE)) &&
                (Fcb->FcbCondition == FcbGood) &&
                !FlagOn(Vcb->VcbState, VCB_STATE_FLAG_WRITE_PROTECTED)) {

                if (!FatIsDirectoryEmpty(IrpContext, Fcb)) {

                    //
                    //  If there are files in the directory at this point,
                    //  forget that we were trying to delete it.
                    //

                    ClearFlag( Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE );

                } else {

                    //
                    //  Even if something goes wrong, we cannot turn back!
                    //
        
                    __try {
        
                        DELETE_CONTEXT DeleteContext;
        
                        //
                        //  Before truncating file allocation remember this
                        //  info for FatDeleteDirent.
                        //
        
                        DeleteContext.FileSize = Fcb->Header.FileSize.LowPart;
                        DeleteContext.FirstClusterOfFile = Fcb->FirstClusterOfFile;
        
                        //
                        //  Synchronize here with paging IO
                        //
        
                        (VOID)ExAcquireResourceExclusiveLite( Fcb->Header.PagingIoResource,
                                                          TRUE );
        
                        Fcb->Header.FileSize.LowPart = 0;
        
                        ExReleaseResourceLite( Fcb->Header.PagingIoResource );
        
                        if (Vcb->VcbCondition == VcbGood) {
        
                            //
                            //  Truncate the file allocation down to zero
                            //
        
                            DebugTrace(0, Dbg, "Delete File allocation\n", 0);
        
                            FatTruncateFileAllocation( IrpContext, Fcb, 0 );

                            if (Fcb->Header.AllocationSize.LowPart == 0) {
        
                                //
                                //  Tunnel and remove the dirent for the directory
                                //
            
                                DebugTrace(0, Dbg, "Delete the directory dirent\n", 0);
            
                                FatTunnelFcbOrDcb( Fcb, NULL );
    
                                FatDeleteDirent( IrpContext, Fcb, &DeleteContext, TRUE );
            
                                //
                                //  Report that we have removed an entry.
                                //
        
                                FatNotifyReportChange( IrpContext,
                                                       Vcb,
                                                       Fcb,
                                                       FILE_NOTIFY_CHANGE_DIR_NAME,
                                                       FILE_ACTION_REMOVED );
                            }
                        }

                    } __except( FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) {
    
                          FatResetExceptionState( IrpContext );
                    }

                    //
                    //  Remove the entry from the name table.
                    //  This will ensure that
                    //  we will not collide with the Dcb if the user wants
                    //  to recreate the same file over again before we
                    //  get a close irp.
                    //
    
                    FatRemoveNames( IrpContext, Fcb );
                }
            }

            //
            //  Decrement the unclean count.
            //

            ASSERT( Fcb->UncleanCount != 0 );
            Fcb->UncleanCount -= 1;

            break;

        case UserFileOpen:

            DebugTrace(0, Dbg, "Cleanup UserFileOpen\n", 0);

            ShareAccess = &Fcb->ShareAccess;

            //
            //  Determine here if we should do a delayed close.
            //

            if ((FileObject->SectionObjectPointer->DataSectionObject == NULL) &&
                (FileObject->SectionObjectPointer->ImageSectionObject == NULL) &&
                (Fcb->UncleanCount == 1) &&
                (Fcb->OpenCount == 1) &&
                !FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE) &&
                !FlagOn(Fcb->FcbState, FCB_STATE_PAGING_FILE) &&
                Fcb->FcbCondition == FcbGood) {

                //
                //  Delay our close.
                //

                SetFlag( Fcb->FcbState, FCB_STATE_DELAY_CLOSE );
            }

            //
            //  Clear the deny defrag bit, if the handle we're cleaning up was the one that set it.
            //
            
            if( FlagOn(Fcb->FcbState, FCB_STATE_DENY_DEFRAG) && FlagOn(Ccb->Flags, CCB_FLAG_DENY_DEFRAG) ) {
                
                ClearFlag(Ccb->Flags, CCB_FLAG_DENY_DEFRAG);
                ClearFlag(Fcb->FcbState, FCB_STATE_DENY_DEFRAG );
            }

            //
            //  Unlock all outstanding file locks.
            //

            (VOID) FsRtlFastUnlockAll( &Fcb->Specific.Fcb.FileLock,
                                       FileObject,
                                       IoGetRequestorProcess( Irp ),
                                       NULL );

            //
            //  We can proceed with on-disk updates only if the volume is mounted.
            //  Remember that we toss all sections in the failed-verify and dismount
            //  cases.
            //
            
            if (Vcb->VcbCondition == VcbGood) {
                
                if (Fcb->FcbCondition == FcbGood) {
                    
                    FatUpdateDirentFromFcb( IrpContext, FileObject, Fcb, Ccb );
                }
    
                //
                //  If the file has a unclean count of 1 then we know
                //  that this is the last handle for the file object.
                //
    
                if ( (Fcb->UncleanCount == 1) && (Fcb->FcbCondition == FcbGood) ) {
    
                    DELETE_CONTEXT DeleteContext;
    
                    //
                    //  Check if we should be deleting the file.  The
                    //  delete operation really deletes the file but
                    //  keeps the Fcb around for close to do away with.
                    //
    
                    if (FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE) &&
                        !FlagOn(Vcb->VcbState, VCB_STATE_FLAG_WRITE_PROTECTED)) {
    
                        //
                        //  Before truncating file allocation remember this
                        //  info for FatDeleteDirent.
                        //
    
                        DeleteContext.FileSize = Fcb->Header.FileSize.LowPart;
                        DeleteContext.FirstClusterOfFile = Fcb->FirstClusterOfFile;
    
                        DebugTrace(0, Dbg, "Delete File allocation\n", 0);
    
                        //
                        //  Synchronize here with paging IO
                        //
    
                        (VOID)ExAcquireResourceExclusiveLite( Fcb->Header.PagingIoResource,
                                                          TRUE );
    
                        Fcb->Header.FileSize.LowPart = 0;
                        Fcb->Header.ValidDataLength.LowPart = 0;
                        Fcb->ValidDataToDisk = 0;
    
                        ExReleaseResourceLite( Fcb->Header.PagingIoResource );
    
                        __try {
    
                            FatSetFileSizeInDirent( IrpContext, Fcb, NULL );
    
                        } __except( FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                                  EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) {
    
                              FatResetExceptionState( IrpContext );
                        }
    
                        Fcb->FcbState |= FCB_STATE_TRUNCATE_ON_CLOSE;
    
                    } else {
    
                        //
                        //  We must zero between ValidDataLength and FileSize
                        //
    
                        if (!FlagOn(Fcb->FcbState, FCB_STATE_PAGING_FILE) &&
                            (Fcb->Header.ValidDataLength.LowPart < Fcb->Header.FileSize.LowPart)) {
    
                            ULONG ValidDataLength;
    
                            ValidDataLength = Fcb->Header.ValidDataLength.LowPart;
    
                            if (ValidDataLength < Fcb->ValidDataToDisk) {
                                ValidDataLength = Fcb->ValidDataToDisk;
                            }
    
                            //
                            //  Recheck, VDD can be >= FS
                            //
                            
                            if (ValidDataLength < Fcb->Header.FileSize.LowPart) {
                                
                                __try {

                                    (VOID)FatZeroData( IrpContext,
                                                       Vcb,
                                                       FileObject,
                                                       ValidDataLength,
                                                       Fcb->Header.FileSize.LowPart -
                                                       ValidDataLength );

                                    //
                                    //  Since we just zeroed this, we can now bump
                                    //  up VDL in the Fcb.
                                    //

                                    Fcb->ValidDataToDisk =
                                    Fcb->Header.ValidDataLength.LowPart =
                                    Fcb->Header.FileSize.LowPart;

                                    //
                                    //  We inform Cc of the motion so that the cache map is updated.
                                    //  This prevents optimized zero-page faults in case the cache
                                    //  structures are re-used for another handle before they are torn
                                    //  down by our soon-to-occur uninitialize. If they were, a noncached
                                    //  producer could write into the region we just zeroed and Cc would
                                    //  be none the wiser, then our async cached reader comes in and takes
                                    //  the optimized path, and we get bad (zero) data.
                                    //
                                    //  If this was memory mapped, we don't have to (can't) tell Cc, it'll
                                    //  figure it out when a cached handle is opened.
                                    //

                                    if (CcIsFileCached( FileObject )) {
                                        CcSetFileSizes( FileObject, (PCC_FILE_SIZES)&Fcb->Header.AllocationSize );
                                    }

                                } __except( FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                                          EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) {

                                      FatResetExceptionState( IrpContext );
                                }
                            }
                        }
                    }
    
                    //
                    //  See if we are supposed to truncate the file on the last
                    //  close.  If we cannot wait we'll ship this off to the fsp
                    //
    
                    __try {
    
                        if (FlagOn(Fcb->FcbState, FCB_STATE_TRUNCATE_ON_CLOSE)) {
    
                            DebugTrace(0, Dbg, "truncate file allocation\n", 0);
    
                            if (Vcb->VcbCondition == VcbGood) {
    
                                FatTruncateFileAllocation( IrpContext,
                                                           Fcb,
                                                           Fcb->Header.FileSize.LowPart );
                            }
    
                            //
                            //  We also have to get rid of the Cache Map because
                            //  this is the only way we have of trashing the
                            //  truncated pages.
                            //
    
                            LocalTruncateSize = Fcb->Header.FileSize;
                            TruncateSize = &LocalTruncateSize;
    
                            //
                            //  Mark the Fcb as having now been truncated, just incase
                            //  we have to reship this off to the fsp.
                            //
    
                            Fcb->FcbState &= ~FCB_STATE_TRUNCATE_ON_CLOSE;
                        }
    
                        //
                        //  Now check again if we are to delete the file and if
                        //  so then we remove the file from the disk.
                        //
    
                        if (FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE) &&
                            Fcb->Header.AllocationSize.LowPart == 0) {
    
                            DebugTrace(0, Dbg, "Delete File\n", 0);
    
                            //
                            //  Now tunnel and delete the dirent
                            //
    
                            FatTunnelFcbOrDcb( Fcb, Ccb );
    
                            FatDeleteDirent( IrpContext, Fcb, &DeleteContext, TRUE );
    
                            //
                            //  Report that we have removed an entry.
                            //
    
                            FatNotifyReportChange( IrpContext,
                                                   Vcb,
                                                   Fcb,
                                                   FILE_NOTIFY_CHANGE_FILE_NAME,
                                                   FILE_ACTION_REMOVED );
                        }
    
                    } __except( FsRtlIsNtstatusExpected(GetExceptionCode()) ?
                              EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH ) {
    
                          FatResetExceptionState( IrpContext );
                    }
    
                    if (FlagOn(Fcb->FcbState, FCB_STATE_DELETE_ON_CLOSE)) {
    
                        //
                        //  Remove the entry from the splay table. This will
                        //  ensure that we will not collide with the Fcb if the
                        //  user wants to recreate the same file over again
                        //  before we get a close irp.
                        //
                        //  Note that we remove the name even if we couldn't
                        //  truncate the allocation and remove the dirent above.
                        //
    
                        FatRemoveNames( IrpContext, Fcb );
                    }
                }
            }
            
            //
            //  We've just finished everything associated with an unclean
            //  fcb so now decrement the unclean count before releasing
            //  the resource.
            //

            ASSERT( Fcb->UncleanCount != 0 );
            Fcb->UncleanCount -= 1;
            if (!FlagOn( FileObject->Flags, FO_CACHE_SUPPORTED )) {
                ASSERT( Fcb->NonCachedUncleanCount != 0 );
                Fcb->NonCachedUncleanCount -= 1;
            }

            //
            //  If this was the last cached open, and there are open
            //  non-cached handles, attempt a flush and purge operation
            //  to avoid cache coherency overhead from these non-cached
            //  handles later.  We ignore any I/O errors from the flush.
            //

            if (FlagOn( FileObject->Flags, FO_CACHE_SUPPORTED ) &&
                (Fcb->NonCachedUncleanCount != 0) &&
                (Fcb->NonCachedUncleanCount == Fcb->UncleanCount) &&
                (Fcb->NonPaged->SectionObjectPointers.DataSectionObject != NULL)) {

                CcFlushCache( &Fcb->NonPaged->SectionObjectPointers, NULL, 0, NULL );
            
                //
                //  Grab and release PagingIo to serialize ourselves with the lazy writer.
                //  This will work to ensure that all IO has completed on the cached
                //  data and we will succesfully tear away the cache section.
                //
                
                ExAcquireResourceExclusiveLite( Fcb->Header.PagingIoResource, TRUE);
                ExReleaseResourceLite( Fcb->Header.PagingIoResource );

                CcPurgeCacheSection( &Fcb->NonPaged->SectionObjectPointers,
                                     NULL,
                                     0,
                                     FALSE );
            }

            //
            //  If the file is invalid, hint to the cache that we should throw everything out.
            //
            
            if ( Fcb->FcbCondition == FcbBad ) {

                TruncateSize = &FatLargeZero;
            }

            //
            //  Cleanup the cache map
            //

            CcUninitializeCacheMap( FileObject, TruncateSize, NULL );

            break;

        default:

#pragma prefast( suppress: 28159, "if the type of open is unknown then things are very bad." )
            FatBugCheck( TypeOfOpen, 0, 0 );
        }

        //
        //  We must clean up the share access at this time, since we may not
        //  get a Close call for awhile if the file was mapped through this
        //  File Object.
        //

        if (ShareAccess != NULL) {

            DebugTrace(0, Dbg, "Cleanup the Share access\n", 0);
            IoRemoveShareAccess( FileObject, ShareAccess );
        }

        if (TypeOfOpen == UserFileOpen) {

            //
            //  Coordinate the cleanup operation with the oplock state.
            //  Cleanup operations can always cleanup immediately.
            //

            FsRtlCheckOplock( &Fcb->Specific.Fcb.Oplock,
                              Irp,
                              IrpContext,
                              NULL,
                              NULL );

            Fcb->Header.IsFastIoPossible = FatIsFastIoPossible( Fcb );
        }

        //
        //  First set the FO_CLEANUP_COMPLETE flag.
        //

        SetFlag( FileObject->Flags, FO_CLEANUP_COMPLETE );

        Status = STATUS_SUCCESS;

        //
        //  Now unpin any repinned Bcbs.
        //

        FatUnpinRepinnedBcbs( IrpContext );

        //
        //  If this was deferred flush media, flush the volume.
        //  We used to do this in lieu of write through for all removable
        //  media.
        //

        if (FlagOn(Vcb->VcbState, VCB_STATE_FLAG_DEFERRED_FLUSH) &&
            !FlagOn(Vcb->VcbState, VCB_STATE_FLAG_WRITE_PROTECTED))  {

            //
            //  Flush the file.
            //

            if ((TypeOfOpen == UserFileOpen) && 
                FlagOn(FileObject->Flags, FO_FILE_MODIFIED)) {

                Status = FatFlushFile( IrpContext, Fcb, Flush );
            }

            //
            //  If that worked ok,  then see if we should flush the FAT as well.
            //

            if (NT_SUCCESS(Status) && Fcb && !FatIsFat12( Vcb) && 
                FlagOn( Fcb->FcbState, FCB_STATE_FLUSH_FAT)) {

                Status = FatFlushFat( IrpContext, Vcb);
            }

            if (!NT_SUCCESS(Status)) {

                FatNormalizeAndRaiseStatus( IrpContext, Status );
            }
        }

    } __finally {

        DebugUnwind( FatCommonCleanup );

        if (AcquiredFcb) { FatReleaseFcb( IrpContext, Fcb ); }
        if (AcquiredVcb) { FatReleaseVcb( IrpContext, Vcb ); }

        if (SendUnlockNotification) {
            
            FsRtlNotifyVolumeEvent( FileObject, FSRTL_VOLUME_UNLOCK );
        }

        //
        //  If this is a normal termination then complete the request
        //

        if (!AbnormalTermination()) {

            FatCompleteRequest( IrpContext, Irp, Status );
        }

        DebugTrace(-1, Dbg, "FatCommonCleanup -> %08lx\n", Status);
    }

    return Status;
}

VOID
FatAutoUnlock (
    IN PIRP_CONTEXT IrpContext,
    IN PVCB Vcb
    )
{
    KIRQL SavedIrql;

    //
    //  Unlock the volume.
    //

    UNREFERENCED_PARAMETER( IrpContext );
    
    IoAcquireVpbSpinLock( &SavedIrql );

    ClearFlag( Vcb->Vpb->Flags, (VPB_LOCKED | VPB_DIRECT_WRITES_ALLOWED) );

    Vcb->VcbState &= ~VCB_STATE_FLAG_LOCKED;
    Vcb->FileObjectWithVcbLocked = NULL;

    IoReleaseVpbSpinLock( SavedIrql );
}


