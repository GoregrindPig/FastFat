/*++

Copyright (c) 1989-2000 Microsoft Corporation

Module Name:

    NameSup.c

Abstract:

    This module implements the Fat Name support routines


--*/

#include "FatProcs.h"

#define Dbg                              (DEBUG_TRACE_NAMESUP)

#ifdef ALLOC_PRAGMA
#pragma alloc_text(PAGE, Fat8dot3ToString)
#pragma alloc_text(PAGE, FatIsNameInExpression)
#pragma alloc_text(PAGE, FatStringTo8dot3)
#pragma alloc_text(PAGE, FatSetFullFileNameInFcb)
#pragma alloc_text(PAGE, FatGetUnicodeNameFromFcb)
#pragma alloc_text(PAGE, FatUnicodeToUpcaseOem)
#pragma alloc_text(PAGE, FatSelectNames)
#pragma alloc_text(PAGE, FatEvaluateNameCase)
#pragma alloc_text(PAGE, FatSpaceInName)
#endif


BOOLEAN
FatIsNameInExpression (
    IN PIRP_CONTEXT IrpContext,
    IN OEM_STRING Expression,
    IN OEM_STRING Name
    )

/*++

Routine Description:

    This routine compare a name and an expression and tells the caller if
    the name is equal to or not equal to the expression.  The input name
    cannot contain wildcards, while the expression may contain wildcards.

Arguments:

    Expression - Supplies the input expression to check against
                 The caller must have already upcased the Expression.

    Name - Supplies the input name to check for.  The caller must have
           already upcased the name.

Return Value:

    BOOLEAN - TRUE if Name is an element in the set of strings denoted
        by the input Expression and FALSE otherwise.

--*/

{
    PAGED_CODE();

    //
    //  Call the appropriate FsRtl routine do to the real work
    //

    return FsRtlIsDbcsInExpression( &Expression,
                                    &Name );

    UNREFERENCED_PARAMETER( IrpContext );
}


VOID
FatStringTo8dot3 (
    __in PIRP_CONTEXT IrpContext,
    __in OEM_STRING InputString,
    __out_bcount(11) PFAT8DOT3 Output8dot3
    )

/*++

Routine Description:

    Convert a string into fat 8.3 format.  The string must not contain
    any wildcards.

Arguments:

    InputString - Supplies the input string to convert

    Output8dot3 - Receives the converted string, the memory must be supplied
        by the caller.

Return Value:

    None.

--*/

{
    ULONG i;
    ULONG j;

    PAGED_CODE();

    DebugTrace(+1, Dbg, "FatStringTo8dot3\n", 0);
    DebugTrace( 0, Dbg, "InputString = %Z\n", &InputString);

    ASSERT( InputString.Length <= 12 );

    //
    //  Make the output name all blanks
    //

    RtlFillMemory( Output8dot3, 11, UCHAR_SP );

    //
    //  Copy over the first part of the file name.  Stop when we get to
    //  the end of the input string or a dot.
    //

    for (i = 0;
         (i < (ULONG)InputString.Length) && (InputString.Buffer[i] != '.') && (i < 11);
         i += 1) {

        (*Output8dot3)[i] = InputString.Buffer[i];
    }

    //
    //  Check if we need to process an extension
    //

    if (i < (ULONG)InputString.Length) {

        //
        //  Make sure we have a dot and then skip over it.
        //

        ASSERT( (InputString.Length - i) <= 4 );
        ASSERT( InputString.Buffer[i] == '.' );

        i += 1;

        //
        //  Copy over the extension.  Stop when we get to the
        //  end of the input string.
        //

        for (j = 8; (i < (ULONG)InputString.Length); j += 1, i += 1) {

            (*Output8dot3)[j] = InputString.Buffer[i];
        }
    }

    //
    //  Before we return check if we should translate the first character
    //  from 0xe5 to 0x5.
    //

    if ((*Output8dot3)[0] == 0xe5) {

        (*Output8dot3)[0] = FAT_DIRENT_REALLY_0E5;
    }

    DebugTrace(-1, Dbg, "FatStringTo8dot3 -> (VOID)\n", 0);

    UNREFERENCED_PARAMETER( IrpContext );

    return;
}


VOID
Fat8dot3ToString (
    __in PIRP_CONTEXT IrpContext,
    __in PDIRENT Dirent,
    __in BOOLEAN RestoreCase,
    __out POEM_STRING OutputString
    )

/*++

Routine Description:

    Convert fat 8.3 format into a string.  The 8.3 name must be well formed.

Arguments:

    Dirent - Supplies the input 8.3 name to convert

    RestoreCase - If TRUE, then the magic reserved bits are used to restore
        the original case.

    OutputString - Receives the converted name, the memory must be supplied
        by the caller.

Return Value:

    None

--*/

{
    ULONG StringIndex;
    ULONG BaseLength, ExtensionLength;

    PAGED_CODE();

    DebugTrace(+1, Dbg, "Fat8dot3ToString\n", 0);

    //
    //  First, find the length of the base component.
    //

    for (BaseLength = 8; BaseLength > 0; BaseLength -= 1) {

        if (Dirent->FileName[BaseLength - 1] != UCHAR_SP) {

            break;
        }
    }

    //
    //  Now find the length of the extension.
    //

    for (ExtensionLength = 3; ExtensionLength > 0; ExtensionLength -= 1) {

        if (Dirent->FileName[8 + ExtensionLength - 1] != UCHAR_SP) {

            break;
        }
    }

    //
    //  If there was a base part, copy it and check the case.  Don't forget
    //  if the first character needs to be changed from 0x05 to 0xe5.
    //

    if (BaseLength != 0) {

        RtlCopyMemory( OutputString->Buffer, Dirent->FileName, BaseLength );

        if (OutputString->Buffer[0] == FAT_DIRENT_REALLY_0E5) {

            OutputString->Buffer[0] = 0xe5;
        }

        //
        //  Now if we are to restore case, look for A-Z
        //

        if (FatData.ChicagoMode &&
            RestoreCase &&
            FlagOn(Dirent->NtByte, FAT_DIRENT_NT_BYTE_8_LOWER_CASE)) {

            for (StringIndex = 0; StringIndex < BaseLength; StringIndex += 1) {

                //
                //  Depending on whether the media was built in a system that was
                //  running with "code page invariance" (see FatEvaluateNameCase),
                //  there could be double-byte OEM characters lying in wait here.
                //  Gotta skip them.
                //
                
                if (FsRtlIsLeadDbcsCharacter(OutputString->Buffer[StringIndex])) {

                    StringIndex += 1;
                    continue;
                }
                
                if ((OutputString->Buffer[StringIndex] >= 'A') &&
                    (OutputString->Buffer[StringIndex] <= 'Z')) {

                    OutputString->Buffer[StringIndex] += 'a' - 'A';
                }
            }
        }
    }

    //
    //  If there was an extension, copy that over.  Else we now know the
    //  size of the string.
    //

    if (ExtensionLength != 0) {

        PUCHAR o, d;

        //
        //  Now add the dot
        //

        OutputString->Buffer[BaseLength++] = '.';

        //
        //  Copy over the extension into the output buffer.
        //

        o = (PUCHAR)&OutputString->Buffer[BaseLength];
        d = &Dirent->FileName[8];

        switch (ExtensionLength) {
        case 3:
            *o++ = *d++;
        case 2:
            *o++ = *d++;
        case 1:
            *o++ = *d++;
        }

        //
        //  Set the output string length
        //

        OutputString->Length = (USHORT)(BaseLength + ExtensionLength);

        //
        //  Now if we are to restore case, look for A-Z
        //

        if (FatData.ChicagoMode &&
            RestoreCase &&
            FlagOn(Dirent->NtByte, FAT_DIRENT_NT_BYTE_3_LOWER_CASE)) {

            for (StringIndex = BaseLength;
                 StringIndex < OutputString->Length;
                 StringIndex++ ) {

                //
                //  Depending on whether the media was built in a system that was
                //  running with "code page invariance" (see FatEvaluateNameCase),
                //  there could be double-byte OEM characters lying in wait here.
                //  Gotta skip them.
                //
                
                if (FsRtlIsLeadDbcsCharacter(OutputString->Buffer[StringIndex])) {

                    StringIndex += 1;
                    continue;
                }
                
                if ((OutputString->Buffer[StringIndex] >= 'A') &&
                    (OutputString->Buffer[StringIndex] <= 'Z')) {

                    OutputString->Buffer[StringIndex] += 'a' - 'A';
                }
            }
        }

    } else {

        //
        //  Set the output string length
        //

        OutputString->Length = (USHORT)BaseLength;
    }

    //
    //  And return to our caller
    //

    DebugTrace(-1, Dbg, "Fat8dot3ToString, OutputString = \"%Z\" -> VOID\n", OutputString);

    UNREFERENCED_PARAMETER( IrpContext );

    return;
}

__drv_mustHoldCriticalRegion
VOID
FatGetUnicodeNameFromFcb (
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb,
    IN OUT PUNICODE_STRING Lfn
    )

/*++

Routine Description:

    This routine will return the unicode name for a given Fcb.  If the
    file has an LFN, it will return this.  Otherwise it will return
    the UNICODE conversion of the Oem name, properly cased.

Arguments:

    Fcb - Supplies the Fcb to query.

    Lfn - Supplies a string that already has enough storage for the
        full unicode name.

Return Value:

    None

--*/

{
    PDIRENT Dirent;
    PBCB DirentBcb = NULL;
    ULONG DirentByteOffset;

    CCB LocalCcb;

    PAGED_CODE();

    ASSERT((MAX_LFN_CHARACTERS * sizeof( WCHAR)) == Lfn->MaximumLength);
    
    //
    //  We'll start by locating the dirent for the name.
    //

    FatStringTo8dot3( IrpContext,
                      Fcb->ShortName.Name.Oem,
                      &LocalCcb.OemQueryTemplate.Constant );

    LocalCcb.Flags = 0;
    LocalCcb.UnicodeQueryTemplate.Length = 0;
    LocalCcb.ContainsWildCards = FALSE;

    FatLocateDirent( IrpContext,
                     Fcb->ParentDcb,
                     &LocalCcb,
                     Fcb->LfnOffsetWithinDirectory,
                     &Dirent,
                     &DirentBcb,
                     (PVBO)&DirentByteOffset,
                     NULL,
                     Lfn);
    __try {

        //
        //  If we didn't find the Dirent, something is terribly wrong.
        //

        if ((DirentBcb == NULL) ||
            (DirentByteOffset != Fcb->DirentOffsetWithinDirectory)) {

            FatRaiseStatus( IrpContext, STATUS_FILE_INVALID );
        }

        //
        //  Check for the easy case.
        //

        if (Lfn->Length == 0) {

            NTSTATUS Status;
            OEM_STRING ShortName;
            UCHAR ShortNameBuffer[12];

            //
            //  If we thought that there was an LFN here and didn't find one,
            //  we're as dead.  This shouldn't happen in normal operation, but
            //  if someone scrambles a directory by hand ...
            //
            
            ASSERT( Fcb->LfnOffsetWithinDirectory == Fcb->DirentOffsetWithinDirectory );

            if (Fcb->LfnOffsetWithinDirectory != Fcb->DirentOffsetWithinDirectory) {

                FatRaiseStatus( IrpContext, STATUS_FILE_INVALID );
            }

            //
            //  There is no LFN, so manufacture a UNICODE name.
            //

            ShortName.Length = 0;
            ShortName.MaximumLength = 12;
            ShortName.Buffer = (PCHAR)ShortNameBuffer;

            Fat8dot3ToString( IrpContext, Dirent, TRUE, &ShortName );

            //
            //  OK, now convert this string to UNICODE
            //

#pragma prefast( suppress:28931, "needed for debug build" )
            Status = RtlOemStringToCountedUnicodeString( Lfn,
                                                         &ShortName,
                                                         FALSE );

            ASSERT( Status == STATUS_SUCCESS );
        }

    } __finally {

        FatUnpinBcb( IrpContext, DirentBcb );
    }
}

__drv_mustHoldCriticalRegion
VOID
FatSetFullFileNameInFcb (
    IN PIRP_CONTEXT IrpContext,
    IN PFCB Fcb
    )

/*++

Routine Description:

    If the FullFileName field in the Fcb has not yet been filled in, we
    proceed to do so.

Arguments:

    Fcb - Supplies the file.

Return Value:

    None

--*/

{
    PAGED_CODE();

    if (Fcb->FullFileName.Buffer == NULL) {

        UNICODE_STRING Lfn;
        PFCB TmpFcb = Fcb;
        PFCB StopFcb;
        PWCHAR TmpBuffer;
        ULONG PathLength = 0;

        //
        //  We will assume we do this infrequently enough, that it's OK to
        //  to a pool allocation here.
        //

        Lfn.Length = 0;
        Lfn.MaximumLength = MAX_LFN_CHARACTERS * sizeof(WCHAR);
        Lfn.Buffer = FsRtlAllocatePoolWithTag( PagedPool,
                                               MAX_LFN_CHARACTERS * sizeof(WCHAR),
                                               TAG_FILENAME_BUFFER );

        __try {

            //
            //  First determine how big the name will be.  If we find an
            //  ancestor with a FullFileName, our work is easier.
            //

            while (TmpFcb != Fcb->Vcb->RootDcb) {

                if ((TmpFcb != Fcb) && (TmpFcb->FullFileName.Buffer != NULL)) {

                    PathLength += TmpFcb->FullFileName.Length;

                    Fcb->FullFileName.Buffer = FsRtlAllocatePoolWithTag( PagedPool,
                                                                         PathLength,
                                                                         TAG_FILENAME_BUFFER );

                    RtlCopyMemory( Fcb->FullFileName.Buffer,
                                   TmpFcb->FullFileName.Buffer,
                                   TmpFcb->FullFileName.Length );

                    break;
                }

                PathLength += TmpFcb->FinalNameLength + sizeof(WCHAR);

                TmpFcb = TmpFcb->ParentDcb;
            }

            //
            //  If FullFileName.Buffer is still NULL, allocate it.
            //

            if (Fcb->FullFileName.Buffer == NULL) {

                Fcb->FullFileName.Buffer = FsRtlAllocatePoolWithTag( PagedPool,
                                                                     PathLength,
                                                                     TAG_FILENAME_BUFFER );
            }

            StopFcb = TmpFcb;

            TmpFcb = Fcb;
            TmpBuffer =  Fcb->FullFileName.Buffer + PathLength / sizeof(WCHAR);

            Fcb->FullFileName.Length =
            Fcb->FullFileName.MaximumLength = (USHORT)PathLength;

            while (TmpFcb != StopFcb) {

                FatGetUnicodeNameFromFcb( IrpContext,
                                          TmpFcb,
                                          &Lfn );

                TmpBuffer -= Lfn.Length / sizeof(WCHAR);

                RtlCopyMemory( TmpBuffer, Lfn.Buffer, Lfn.Length );

                TmpBuffer -= 1;

                *TmpBuffer = L'\\';

                TmpFcb = TmpFcb->ParentDcb;
            }

        } __finally {

            if (AbnormalTermination()) {

                if (Fcb->FullFileName.Buffer) {

                    ExFreePool( Fcb->FullFileName.Buffer );
                    Fcb->FullFileName.Buffer = NULL;
                }
            }

            ExFreePool( Lfn.Buffer );
        }
    }
}

VOID
FatUnicodeToUpcaseOem (
    IN PIRP_CONTEXT IrpContext,
    IN POEM_STRING OemString,
    IN PUNICODE_STRING UnicodeString
    )

/*++

Routine Description:

    This routine is our standard routine for trying to use stack space
    if possible when calling RtlUpcaseUnicodeStringToCountedOemString().

    If an unmappable character is encountered, we set the destination
    length to 0.

Arguments:

    OemString - Specifies the destination string.  Space is already assumed to
        be allocated.  If there is not enough, then we allocate enough
        space.

    UnicodeString - Specifies the source string.

Return Value:

    None.

--*/

{
    NTSTATUS Status;

    PAGED_CODE();

    Status = RtlUpcaseUnicodeStringToCountedOemString( OemString,
                                                       UnicodeString,
                                                       FALSE );

    if (Status == STATUS_BUFFER_OVERFLOW) {

        OemString->Buffer = NULL;
        OemString->Length = 0;
        OemString->MaximumLength = 0;

        Status = RtlUpcaseUnicodeStringToCountedOemString( OemString,
                                                           UnicodeString,
                                                           TRUE );
    }

    if (!NT_SUCCESS(Status)) {

        if (Status == STATUS_UNMAPPABLE_CHARACTER) {

            OemString->Length = 0;

        } else {

            FatNormalizeAndRaiseStatus( IrpContext, Status );
        }
    }

    return;
}


__drv_mustHoldCriticalRegion
VOID
FatSelectNames (
    IN PIRP_CONTEXT IrpContext,
    IN PDCB Parent,
    IN POEM_STRING OemName,
    IN PUNICODE_STRING UnicodeName,
    IN OUT POEM_STRING ShortName,
    IN PUNICODE_STRING SuggestedShortName OPTIONAL,
    IN OUT BOOLEAN *AllLowerComponent,
    IN OUT BOOLEAN *AllLowerExtension,
    IN OUT BOOLEAN *CreateLfn
    )

/*++

Routine Description:

    This routine takes the original UNICODE string that the user specified,
    and the upcased Oem equivolent.  This routine then decides if the OemName
    is acceptable for dirent, or whether a short name has to be manufactured.

    Two values are returned to the caller.  One tells the caller if the name
    happens to be all lower case < 0x80.  In this special case we don't
    have to create an Lfn.  Also we tell the caller if it must create an LFN.

Arguments:

    OemName -  Supplies the proposed short Oem name.

    ShortName - If this OemName is OK for storeage in a dirent it is copied to
        this string, otherwise this string is filled with a name that is OK.
        If OemName and ShortName are the same string, no copy is done.

    UnicodeName - Provides the original final name.

    SuggestedShortName - a first-try short name to try before auto-generation
        is used

    AllLowerComponent - Returns whether this component was all lower case.

    AllLowerExtension - Returns wheather the extension was all lower case.

    CreateLfn - Tells the caller if we must create an LFN for the UnicodeName or
        SuggestedLongName

Return Value:

    None.

--*/

{
    BOOLEAN GenerateShortName;
    
    PAGED_CODE();

    //
    //  First see if we must generate a short name.
    //

    if ((OemName->Length == 0) ||
        !FatIsNameShortOemValid( IrpContext, *OemName, FALSE, FALSE, FALSE ) ||
        FatSpaceInName( IrpContext, UnicodeName )) {

        WCHAR ShortNameBuffer[12];
        UNICODE_STRING ShortUnicodeName;
        GENERATE_NAME_CONTEXT Context;
        BOOLEAN TrySuggestedShortName;

        PDIRENT Dirent;
        PBCB Bcb = NULL;
        ULONG ByteOffset;
        NTSTATUS Status;

        GenerateShortName = TRUE;

        TrySuggestedShortName = (SuggestedShortName != NULL);
    
        //
        //  Now generate a short name.
        //

        ShortUnicodeName.Length = 0;
        ShortUnicodeName.MaximumLength = 12 * sizeof(WCHAR);
        ShortUnicodeName.Buffer = ShortNameBuffer;

        RtlZeroMemory( &Context, sizeof( GENERATE_NAME_CONTEXT ) );

        __try {
            
            while ( TRUE ) {

                FatUnpinBcb( IrpContext, Bcb );

                if (TrySuggestedShortName) {

                    //
                    //  Try our caller's candidate first. Note that this must have
                    //  been uppercased previously.
                    //

                    ShortUnicodeName.Length = SuggestedShortName->Length;
                    ShortUnicodeName.MaximumLength = SuggestedShortName->MaximumLength;
                    ShortUnicodeName.Buffer = SuggestedShortName->Buffer;

                    TrySuggestedShortName = FALSE;

                } else {

                    RtlGenerate8dot3Name( UnicodeName, TRUE, &Context, &ShortUnicodeName );
                }

                //
                //  We have a candidate, make sure it doesn't exist.
                //

#pragma prefast( suppress:28931, "needed for debug build" )
                Status = RtlUnicodeStringToCountedOemString( ShortName,
                                                             &ShortUnicodeName,
                                                             FALSE );

                ASSERT( Status == STATUS_SUCCESS );

                FatLocateSimpleOemDirent( IrpContext,
                                          Parent,
                                          ShortName,
                                          &Dirent,
                                          &Bcb,
                                          (PVBO)&ByteOffset );

                if (Bcb == NULL) {

                    __leave;

                }
            }

        } __finally {
            
            FatUnpinBcb( IrpContext, Bcb );
        }

    } else {

        //
        //  Only do this copy if the two string are indeed different.
        //

        if (ShortName != OemName) {

            ShortName->Length = OemName->Length;
            RtlCopyMemory( ShortName->Buffer, OemName->Buffer, OemName->Length );
        }

        GenerateShortName = FALSE;
    }

    //
    //  Now see if the caller will have to use unicode string as an LFN
    //

    if (GenerateShortName) {

        *CreateLfn = TRUE;
        *AllLowerComponent = FALSE;
        *AllLowerExtension = FALSE;

    } else {

        FatEvaluateNameCase( IrpContext,
                             UnicodeName,
                             AllLowerComponent,
                             AllLowerExtension,
                             CreateLfn );
    }

    return;
}


VOID
FatEvaluateNameCase (
    IN PIRP_CONTEXT IrpContext,
    IN PUNICODE_STRING UnicodeName,
    IN OUT BOOLEAN *AllLowerComponent,
    IN OUT BOOLEAN *AllLowerExtension,
    IN OUT BOOLEAN *CreateLfn
    )

/*++

Routine Description:

    This routine takes a UNICODE string and sees if it is eligible for
    the special case optimization.

Arguments:

    UnicodeName - Provides the original final name.

    AllLowerComponent - Returns whether this compoent was all lower case.

    AllLowerExtension - Returns wheather the extension was all lower case.

    CreateLfn - Tells the call if we must create an LFN for the UnicodeName.

Return Value:

    None.

--*/

{
    ULONG i;
    UCHAR Uppers = 0;
    UCHAR Lowers = 0;

    BOOLEAN ExtensionPresent = FALSE;

    PAGED_CODE();
    UNREFERENCED_PARAMETER( IrpContext );
    
    *CreateLfn = FALSE;

    for (i = 0; i < UnicodeName->Length / sizeof(WCHAR); i++) {

        WCHAR c;

        c = UnicodeName->Buffer[i];

        if ((c >= 'A') && (c <= 'Z')) {

            Uppers += 1;

        } else if ((c >= 'a') && (c <= 'z')) {

            Lowers += 1;

        } else if ((c >= 0x0080) && FatData.CodePageInvariant) {

            break;
        }

        //
        //  If we come to a period, figure out if the extension was
        //  all one case.
        //

        if (c == L'.') {

            *CreateLfn = (Lowers != 0) && (Uppers != 0);

            *AllLowerComponent = !(*CreateLfn) && (Lowers != 0);

            ExtensionPresent = TRUE;

            //
            //  Now reset the uppers and lowers count.
            //

            Uppers = Lowers = 0;
        }
    }

    //
    //  Now check again for creating an LFN.
    //

    *CreateLfn = (*CreateLfn ||
                  (i != UnicodeName->Length / sizeof(WCHAR)) ||
                  ((Lowers != 0) && (Uppers != 0)));

    //
    //  Now we know the final state of CreateLfn, update the two
    //  "AllLower" booleans.
    //

    if (ExtensionPresent) {

        *AllLowerComponent = !(*CreateLfn) && *AllLowerComponent;
        *AllLowerExtension = !(*CreateLfn) && (Lowers != 0);

    } else {

        *AllLowerComponent = !(*CreateLfn) && (Lowers != 0);
        *AllLowerExtension = FALSE;
    }

    return;
}


BOOLEAN
FatSpaceInName (
    IN PIRP_CONTEXT IrpContext,
    IN PUNICODE_STRING UnicodeName
    )

/*++

Routine Description:

    This routine takes a UNICODE string and sees if it contains any spaces.

Arguments:

    UnicodeName - Provides the final name.

Return Value:

    BOOLEAN - TRUE if it does, FALSE if it doesn't.

--*/

{
    ULONG i;

    PAGED_CODE();
    UNREFERENCED_PARAMETER( IrpContext );
    
    for (i=0; i < UnicodeName->Length/sizeof(WCHAR); i++) {

        if (UnicodeName->Buffer[i] == L' ') {
            return TRUE;
        }
    }

    return FALSE;
}


