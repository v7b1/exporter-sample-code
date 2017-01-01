#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"
#include "ext_common.h"
#include "ext_buffer.h"

#import <CoreFoundation/CoreFoundation.h>
#include <AudioToolbox/AudioToolbox.h>
#include <stdlib.h>

/*
	example code:
	use core audio to export buffer contents to an audiofile with compressed format
	author: volker böhm, july 2016
*/


typedef struct {
	t_object		b_obj;
	
	t_buffer_ref	*bufref;
	t_symbol        *bufname;			// buffer name
	long			frames;
	void			*outA;
	double			sr;
	
	CFStringRef outputFileStringRef;
	UInt32			fileType;
	UInt16			bitRate;

} t_myObj;


static t_class *myObj_class;

void myObj_setBitRate(t_myObj *x, long bitrate);
void myObj_export(t_myObj *x);
void myObj_dosave(t_myObj *x);
void do_export(t_myObj *x);
void myObj_bang(t_myObj *x);

int convertAndWriteToDisk(float* pcmData, int frames, short nchnls, ExtAudioFileRef outputFileRef);
int checkConverterSettings(AudioStreamBasicDescription sourceFormat, AudioStreamBasicDescription destFormat);
void createCanonicalFormat(AudioStreamBasicDescription *format, double sr, int nchnls);
void createM4aFormat(AudioStreamBasicDescription *format, double sr, int nchnls);

void set_bitRate(ExtAudioFileRef outputFileRef, UInt32 bitRate);
void myObj_setBuf(t_myObj *x, t_symbol *s);
void myObj_dblclick(t_myObj *x);
t_max_err myObj_notify(t_myObj *x, t_symbol *s, t_symbol *msg, void *sender, void *data);
void myObj_free(t_myObj *x);
void myObj_assist(t_myObj *x, void *b, long m, long a, char *s);
void *myObj_new( t_symbol *s, long argc, t_atom *argv);


int C74_EXPORT main(void) {
	t_class *c;
	
	c = class_new("exporter~", (method)myObj_new, (method)myObj_free, (short)sizeof(t_myObj),
				  0L, A_GIMME, 0L);
	class_addmethod(c, (method)myObj_setBitRate, "bitrate", A_LONG, 0);
    class_addmethod(c, (method)myObj_bang, "bang", 0);
	class_addmethod(c, (method)myObj_export, "export", 0);
	class_addmethod(c, (method)myObj_setBuf, "set", A_SYM, 0);
	class_addmethod(c, (method)myObj_dblclick, "dblclick", A_CANT, 0);
	class_addmethod(c, (method)myObj_assist, "assist", A_CANT,0);
	class_addmethod(c, (method)myObj_notify, "notify", A_CANT, 0);


	class_register(CLASS_BOX, c);
	myObj_class = c;
	
	post("exporter~ save referenced buffer contents to audiofle with compressed format");
	
	return 0;
}



#pragma mark user functions ---------------

void myObj_setBitRate(t_myObj *x, long bitrate) {
	if (bitrate >= 0) x->bitRate = bitrate;
	else x->bitRate = 0;
}


void myObj_bang(t_myObj *x) {
	x->fileType = kAudioFileM4AType;
	x->bitRate = 0;
	defer_low(x, (method)myObj_dosave, 0L, 0, 0L);
}


void myObj_export(t_myObj *x) {
	defer_low(x, (method)myObj_dosave, 0L, 0, 0L);
}


void myObj_dosave(t_myObj *x)
{
	OSStatus err;
	short	pathID;
	char		filename[MAX_FILENAME_CHARS];
	char		filepath[MAX_PATH_CHARS];
	
	strcpy(filename, "untitled.m4a");
	if( saveas_dialog(filename, &pathID, NULL) )
		return;

	
	// create POSIX filepath
	err = path_toabsolutesystempath(pathID, filename, filepath);
	if(err) {
		object_error((t_object*)x, "can't create systempath");
		return;
	}
	object_post((t_object *)x, "exporting to: %s", filepath);
	
	x->outputFileStringRef = CFStringCreateWithFileSystemRepresentation(kCFAllocatorDefault, filepath);

	defer_low(x, (method)do_export, 0L, 0, 0L);

}


#pragma mark others------------------


void do_export(t_myObj *x) {

	t_float		*tab;
	t_float		sr;
	double			exportSR = 0;				// leave this at zero for automatic selection of default sr
	long				frames, nchnls;
	t_buffer_obj	*buf;
	
	OSStatus	err;
	AudioStreamBasicDescription sourceFormat = {0};
	AudioStreamBasicDescription exportFormat = {0};
    
	
	buf = buffer_ref_getobject(x->bufref);
	if(!buf) {
		object_error((t_object *)x,"%s is no valid source buffer", x->bufname->s_name);
		return;
	}
	frames = buffer_getframecount(buf);		// number of audio frames in samples
	nchnls = buffer_getchannelcount(buf);		// number of channels
	sr = buffer_getsamplerate(buf);					// get sample rate
    
	// fill in the source format
	createCanonicalFormat(&sourceFormat, sr, nchnls);
	
    // fill in the destination format
	createM4aFormat(&exportFormat, exportSR, nchnls);


	// create output audio file ------------------------
	CFURLRef outputFileURL = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
														   x->outputFileStringRef,
														   kCFURLPOSIXPathStyle,
														   false);

	
    ExtAudioFileRef outputFileRef;
	err = ExtAudioFileCreateWithURL((CFURLRef)outputFileURL,
                                    x->fileType,
                                    &exportFormat,
                                    NULL,
                                    kAudioFileFlags_EraseFile,
								 &outputFileRef);
	if (err) {
		object_error((t_object *)x, "ExtAudioFileCreateWithURL failed -- err: %ld", err);
		return;
	}
	
	CFRelease(outputFileURL);
    
    
    // tell the ExtAudioFile API what format we'll be sending samples in!!!!!!!!!!!!
    err = ExtAudioFileSetProperty(outputFileRef,
                                  kExtAudioFileProperty_ClientDataFormat,
                                  sizeof(sourceFormat),
                                  &sourceFormat);

	
	// set bit rate (in kbps)
	if (x->bitRate > 0) {
		object_post((t_object *)x, "--> bit rate: %d", x->bitRate);
		set_bitRate(outputFileRef, x->bitRate*1000);
	}
	
	
	tab = buffer_locksamples(buf);				// access samples in buffer
	if (!tab) { goto out; }
	
    int result = convertAndWriteToDisk(tab, frames, nchnls, outputFileRef);
    if (result) {
		object_error((t_object *)x, "sorry, can't write packets to file - aborting...");
	}
	
	
    // clean up, we're done!
	buffer_unlocksamples(buf);
	
	outlet_bang(x->outA);
	
out:
    ExtAudioFileDispose(outputFileRef);
	
}


int convertAndWriteToDisk(float* pcmData, int frames, short nchnls, ExtAudioFileRef outputFileRef)
{
	OSStatus err;
	int b, offset;
	UInt32 frameCount = 8192;							// number of frames to write in one block
    UInt32 frameBlocks = frames / frameCount;		// number of _complete_ blocks
    UInt32 frameRest = frames - (frameBlocks * frameCount);		// last block is smaller
    
	// wrap the destination buffer in an AudioBufferList
	AudioBufferList convertedData;
	convertedData.mNumberBuffers = 1;
	convertedData.mBuffers[0].mNumberChannels = nchnls;
	convertedData.mBuffers[0].mDataByteSize = frameCount * nchnls * sizeof(float);
	
	for(b=0; b<frameBlocks; b++) {
		offset = b*frameCount*nchnls;
		convertedData.mBuffers[0].mData = pcmData+offset;
		
		err = ExtAudioFileWrite(outputFileRef,frameCount, &convertedData);
		if(err) { post("Couldn't write packets to file"); return 1; }
	}
	
	// and copy the rest
    offset += (frameCount*nchnls);
	convertedData.mBuffers[0].mData = pcmData+offset;
    convertedData.mBuffers[0].mDataByteSize = frameRest * nchnls * sizeof(float);  // have to change byte size!
    err = ExtAudioFileWrite(outputFileRef, frameRest, &convertedData);
	if(err) {
		post("Couldn't write last packet to file"); return 1;
	}
    
    return 0;
}



#pragma mark AudioStreamBasicDescriptions

void createCanonicalFormat(AudioStreamBasicDescription *format, double sr, int nchnls)
{
	// 32bit floating point
	format->mFormatID = kAudioFormatLinearPCM;
    format->mChannelsPerFrame = nchnls;
    format->mSampleRate = sr;
	format->mFormatFlags = kAudioFormatFlagsCanonical;
    format->mFramesPerPacket = 1;	// uncompressed is always 1 packet per frame
	format->mBitsPerChannel = 32;
	format->mBytesPerPacket =  format->mBytesPerFrame = 4 * nchnls;
}

void createM4aFormat(AudioStreamBasicDescription *format, double sr, int nchnls)
{
	// m4a
	format->mFormatID = kAudioFormatMPEG4AAC;
    format->mChannelsPerFrame = nchnls;
    format->mSampleRate = sr;
	format->mFormatFlags = kMPEG4Object_AAC_Main;
    // leave out the rest
}



#pragma mark utility functions ---------------------

void set_bitRate(ExtAudioFileRef outputFileRef, UInt32 bitRate)
{
	// set Bit Rate ----------------------------------------------------------
	OSStatus err = noErr;
	AudioConverterRef outConvRef;
	UInt32 size = sizeof(outConvRef);
	err = ExtAudioFileGetProperty(outputFileRef, kExtAudioFileProperty_AudioConverter, &size, &outConvRef);
	AudioConverterSetProperty(outConvRef, kAudioConverterEncodeBitRate, sizeof(bitRate), &bitRate);
	//AudioConverterDispose(outConvRef);
}



void myObj_setBuf(t_myObj *x, t_symbol *s)
{
	if (!x->bufref)
		x->bufref = buffer_ref_new((t_object*)x, s);
	else
		buffer_ref_set(x->bufref, s);
	
	x->bufname = s;			
}




void myObj_dblclick(t_myObj *x)
{
	buffer_view(buffer_ref_getobject(x->bufref));
}


t_max_err myObj_notify(t_myObj *x, t_symbol *s, t_symbol *msg, void *sender, void *data)
{
		return buffer_ref_notify(x->bufref, s, msg, sender, data);
}


void myObj_free(t_myObj *x) {
	// free buffer reference
	object_free(x->bufref);
}



void *myObj_new( t_symbol *s, long argc, t_atom *argv)
{
	t_myObj *x = object_alloc(myObj_class);
	t_symbol *bufname = NULL;
	
	if(x) {		
		x->outA = bangout(x);
		x->bufref = NULL;
		

		if(argc>=1) {
			if(atom_gettype(argv) == A_SYM)
				bufname = atom_getsym(argv);
			else
				object_error((t_object *)x, "missing buffer name argument!");
		}
		
		x->fileType = kAudioFileM4AType;
		x->bitRate = 0;

		myObj_setBuf(x, bufname);
		
		// get current sampling rate
		x->sr = sys_getsr();
		if(x->sr <= 0)
			x->sr = 44100.;
		
	}
	
	else {
		object_free(x);
		x = NULL;
	}
	
	
	return x;
}


void myObj_assist(t_myObj *x, void *b, long m, long a, char *s) {
	if (m == ASSIST_INLET) {
		switch(a) {
			case 0: sprintf (s,"bang: start exporting"); break;
		}
	}
	else {
		switch(a) {
			case 0: sprintf (s,"(bang) bang when done"); break;
		}
		
	}
}
