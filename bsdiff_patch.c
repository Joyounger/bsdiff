#include "bsdiff_patch.h"
#include <stdlib.h>
#include <stdio.h>
#include "bzlib.h"

//------------------------------------------------------------------------------

static int readFile(
    FILE *fp, 
    unsigned char *buf, 
    size_t len
    );
static int writeFile(
    FILE *fp, 
    const unsigned char *buf, 
    size_t len
    );
static int readHeader(
    FILE *fp, 
    int *controlBlockSize, 
    int *diffBlockSize, 
    int *newFileSize
    );
static int getFileSize(
    FILE *fp, 
    int *fileSize
    );
static int readOffset(
    unsigned char buf[8]
    );
static void writeError(
    char error[64], 
    const char *str
    );

//------------------------------------------------------------------------------

int bsdiff_patch(const char *oldFile, const char *patchFile, const char *newFile, char error[64])
{
    int retCode = 0;
    FILE *fp = NULL, *fpControl = NULL, *fpDiff = NULL, *fpExtra = NULL;
    BZFILE *bfpControl = NULL, *bfpDiff = NULL, *bfpExtra = NULL;
    unsigned char *oldFileBuf = NULL, *newFileBuf = NULL;
    int controlBlockSize, diffBlockSize, newFileSize, oldFileSize;
    int oldPos, newPos;
    int bzError, bzReaded;
    int i, ctrl[3];
    unsigned char temp[24];

    /* �ļ���ʽ�������£�
       offset  len
        0       8   --> "BSDIFF40"
        8       8   --> X, length of the bzip2(control block)
        16      8   --> Y, length of the bzip2(diff block)
        24      8   --> newfile size
        32      X   --> bzip2(control block)
        32+X    Y   --> bzip2(diff block)
        32+X+Y  ?   --> bzip2(extra block)

        ����control block��һϵ�е�3Ԫ��(x,y,z)��xyz��Ϊ8�ֽڵ��޷����������京��Ϊ��
        add x bytes from oldfile to x bytes from the diff block;
        copy y bytes from the extra block;
        seek forwards in oldfile by z bytes;

       ���ƣ�
       �ڵ�ǰʵ���У�������һ���Է����ڴ�buffer�ķ�ʽ����������32λ���̵�2GB�û�̬��ַ�ռ�����ƣ�
       �޷����������2GB��buffer�����������汾�в�֧�ִ���2GB���ļ���
    */

    // ��patch�ļ�����ȡ��У���ļ�ͷ
    fp = fopen(patchFile, "rb");
    if (!fp) {
        writeError(error, "Can't open patchFile");
        goto MyExit;
    }
    if (!readHeader(fp, &controlBlockSize, &diffBlockSize, &newFileSize)) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }

    // ������BZ2���ļ�������ֱ��ȡpatch�ļ�����������
    fpControl = fp;  // fp��posӦ�øպþ���32����
    fp = NULL;
    bfpControl = BZ2_bzReadOpen(&bzError, fpControl, 0, 0, NULL, 0);
    if (!bfpControl) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }

    fpDiff = fopen(patchFile, "rb");
    if (!fpDiff || fseek(fpDiff, 32 + controlBlockSize, SEEK_SET)) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }
    bfpDiff = BZ2_bzReadOpen(&bzError, fpDiff, 0, 0, NULL, 0);
    if (!bfpDiff) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }

    fpExtra = fopen(patchFile, "rb");
    if (!fpExtra || fseek(fpExtra, 32 + controlBlockSize + diffBlockSize, SEEK_SET)) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }
    bfpExtra = BZ2_bzReadOpen(&bzError, fpExtra, 0, 0, NULL, 0);
    if (!bfpDiff) {
        writeError(error, "Invalid patchFile");
        goto MyExit;
    }

    // ��ȡoldFile���ݵ�oldFileBuf
    fp = fopen(oldFile, "rb");
    if (!fp || !getFileSize(fp, &oldFileSize)) {
        writeError(error, "Can't open oldFile");
        goto MyExit;
    }
    oldFileBuf = (unsigned char*)malloc(oldFileSize);
    if (!oldFileBuf) {
        writeError(error, "Out of memory");
        goto MyExit;
    }
    if (!readFile(fp, oldFileBuf, oldFileSize)) {
        writeError(error, "Failed to read oldFile");
        goto MyExit;
    }
    fclose(fp);
    fp = NULL;

    // ����newFileBuf
    newFileBuf = (unsigned char*)malloc(newFileSize);
    if (!newFileBuf) {
        writeError(error, "Out of memory");
        goto MyExit;
    }

    // ��ʼѭ������
    oldPos = 0;
    newPos = 0;
    while (newPos < newFileSize) {
        // ��Control data
        bzReaded = BZ2_bzRead(&bzError, bfpControl, temp, 24);
        if ((bzError != BZ_OK && bzError != BZ_STREAM_END) || bzReaded != 24) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }
        ctrl[0] = readOffset(temp);
        ctrl[1] = readOffset(temp + 8);
        ctrl[2] = readOffset(temp + 16);
        
        // ��diff�����ж�ctrl[0]���ֽ�
        if (ctrl[0] < 0 || newPos + ctrl[0] > newFileSize) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }
        bzReaded = BZ2_bzRead(&bzError, bfpDiff, newFileBuf + newPos, ctrl[0]);
        if ((bzError != BZ_OK && bzError != BZ_STREAM_END) || bzReaded != ctrl[0]) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }

        // �Ӿ��ļ������ж�ȡctrl[0]���ֽڣ���diff���ݽ��мӲ���
        for (i = 0; i < ctrl[0]; ++i) {
            if (oldPos + i < oldFileSize) {
                newFileBuf[newPos + i] += oldFileBuf[oldPos + i];
            }
        }

        // ����pos
        newPos += ctrl[0];
        oldPos += ctrl[0];

        // ��extra�����ж�ȡctrl[1]���ֽ�
        if (ctrl[1] < 0 || newPos + ctrl[1] > newFileSize) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }
        bzReaded = BZ2_bzRead(&bzError, bfpExtra, newFileBuf + newPos, ctrl[1]);
        if ((bzError != BZ_OK && bzError != BZ_STREAM_END) || bzReaded != ctrl[1]) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }

        // ����pos
        if (ctrl[2] < 0) {
            writeError(error, "Invalid patchFile");
            goto MyExit;
        }
        newPos += ctrl[1];
        oldPos += ctrl[2];
    }

    // ��newFileBuf�е�����д����newFile
    fp = fopen(newFile, "wb");
    if (!fp) {
        writeError(error, "Can't open newFile");
        goto MyExit;
    }
    if (!writeFile(fp, newFileBuf, newFileSize)) {
        writeError(error, "Failed to write newFile");
        goto MyExit;
    }
    fclose(fp);
    fp = NULL;

    // Done
    retCode = 1;

MyExit:
    if (oldFileBuf)
        free(oldFileBuf);
    if (newFileBuf)
        free(newFileBuf);
    if (bfpControl)
        BZ2_bzReadClose(&bzError, bfpControl);
    if (bfpDiff)
        BZ2_bzReadClose(&bzError, bfpDiff);
    if (bfpExtra)
        BZ2_bzReadClose(&bzError, bfpExtra);
    if (fp)
        fclose(fp);
    if (fpControl)
        fclose(fpControl);
    if (fpDiff)
        fclose(fpDiff);
    if (fpExtra)
        fclose(fpExtra);
    return retCode;
}

//------------------------------------------------------------------------------

static int readFile(FILE *fp, unsigned char *buf, size_t len)
{
    size_t n;

    while (len) {
        n = fread(buf, 1, len, fp);
        if (!n)
            return 0;
        buf += n;
        len -= n;
    }
    return 1;
}

static int writeFile(FILE *fp, const unsigned char *buf, size_t len)
{
    size_t n;
    
    while (len) {
        n = fwrite(buf, 1, len, fp);
        if (!n)
            return 0;
        buf += n;
        len -= n;
    }
    return 1;
}

static int readHeader(
    FILE *fp, 
    int *controlBlockSize, 
    int *diffBlockSize, 
    int *newFileSize
    )
{
    unsigned char header[32];

    if (fread(header, 1, 32, fp) != 32)
        return 0;
    
    if (memcmp(header, "BSDIFF40", 8) != 0)
        return 0;

    *controlBlockSize = readOffset(header + 8);
    *diffBlockSize = readOffset(header + 16);
    *newFileSize = readOffset(header + 24);
    if (*controlBlockSize < 0 || *diffBlockSize < 0 || *newFileSize < 0)
        return 0;

    return 1;
}

static int getFileSize(FILE *fp, int *fileSize)
{
    if (fseek(fp, 0, SEEK_END))
        return 0;
    
    *fileSize = ftell(fp);
    if (*fileSize < 0)
        return 0;
    
    if (fseek(fp, 0, SEEK_SET))
        return 0;
    
    return 1;
}

static int readOffset(unsigned char buf[8])
{
    unsigned int off = 0;

    if (buf[7] || buf[6] || buf[5] || buf[4])
        return -1;

    off += buf[3];
    off = off * 256; off += buf[2];
    off = off * 256; off += buf[1];
    off = off * 256; off += buf[0];
    if (off > 0x7fffffff)
        return -1;

    return (int)off;
}

static void writeError(char error[64], const char *str)
{
    int i;
    
    if (error) {
        for (i = 0; i < 63; ++i) {
            if (!str[i])
                break;
            error[i] = str[i];
        }
        error[i] = '\0';
    }
}

//------------------------------------------------------------------------------

#define TEST_BSDIFF

#ifdef TEST_BSDIFF

int main(int argc,char * argv[])
{
    char error[64];

    if (argc != 4) {
        printf("usage: %s oldfile newfile patchfile\n", argv[0]);
        return 1;
    }

    if (!bsdiff_patch(argv[1], argv[3], argv[2], error)) {
        printf("patch failed! error = %s\n", error);
        return 1;
    }
    printf("patch OK\n");
    return 0;
}

#endif // TEST_BSDIFF
