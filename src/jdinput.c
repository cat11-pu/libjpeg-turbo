/*
 * jdinput.c
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-1997, Thomas G. Lane.
 * Lossless JPEG Modifications:
 * Copyright (C) 1999, Ken Murchison.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2010, 2016, 2018, 2022, 2024, D. R. Commander.
 * Copyright (C) 2015, Google, Inc.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains input control logic for the JPEG decompressor.
 * These routines are concerned with controlling the decompressor's input
 * processing (marker reading and coefficient/difference decoding).
 * The actual input reading is done in jdmarker.c, jdhuff.c, jdphuff.c,
 * and jdlhuff.c.
 */

#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jpegapicomp.h"
#include <setjmp.h>


/* Private state */

typedef struct {
  struct jpeg_input_controller pub; /* public fields */

  boolean inheaders;            /* TRUE until first SOS is reached */
  boolean in_scan;              /* TRUE while consuming a scan's entropy data */
  jmp_buf scan_err_jmp;         /* setjmp buffer for scan error recovery */
  void (*saved_error_exit) (j_common_ptr cinfo); /* application's error_exit */
} my_input_controller;

typedef my_input_controller *my_inputctl_ptr;


/* Forward declarations */
METHODDEF(int) consume_markers(j_decompress_ptr cinfo);


/*
 * Routines to calculate various quantities related to the size of the image.
 */

LOCAL(void)
initial_setup(j_decompress_ptr cinfo)
/* Called once, when first SOS marker is reached */
{
  int ci;
  jpeg_component_info *compptr;
  int data_unit = cinfo->master->lossless ? 1 : DCTSIZE;

  /* Make sure image isn't bigger than I can handle */
  if ((long)cinfo->image_height > (long)JPEG_MAX_DIMENSION ||
      (long)cinfo->image_width > (long)JPEG_MAX_DIMENSION)
    ERREXIT1(cinfo, JERR_IMAGE_TOO_BIG, (unsigned int)JPEG_MAX_DIMENSION);

  /* Lossy JPEG images must have 8 or 12 bits per sample.  Lossless JPEG images
   * can have 2 to 16 bits per sample.
   */
#ifdef D_LOSSLESS_SUPPORTED
  if (cinfo->master->lossless) {
    if (cinfo->data_precision < 2 || cinfo->data_precision > 16)
      ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  } else
#endif
  {
    if (cinfo->data_precision != 8 && cinfo->data_precision != 12)
      ERREXIT1(cinfo, JERR_BAD_PRECISION, cinfo->data_precision);
  }

  /* Check that number of components won't exceed internal array sizes */
  if (cinfo->num_components > MAX_COMPONENTS)
    ERREXIT2(cinfo, JERR_COMPONENT_COUNT, cinfo->num_components,
             MAX_COMPONENTS);

  /* Compute maximum sampling factors; check factor validity */
  cinfo->max_h_samp_factor = 1;
  cinfo->max_v_samp_factor = 1;
  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
    if (compptr->h_samp_factor <= 0 ||
        compptr->h_samp_factor > MAX_SAMP_FACTOR ||
        compptr->v_samp_factor <= 0 ||
        compptr->v_samp_factor > MAX_SAMP_FACTOR)
      ERREXIT(cinfo, JERR_BAD_SAMPLING);
    cinfo->max_h_samp_factor = MAX(cinfo->max_h_samp_factor,
                                   compptr->h_samp_factor);
    cinfo->max_v_samp_factor = MAX(cinfo->max_v_samp_factor,
                                   compptr->v_samp_factor);
  }

#if JPEG_LIB_VERSION >= 80
  cinfo->block_size = data_unit;
  cinfo->natural_order = jpeg_natural_order;
  cinfo->lim_Se = DCTSIZE2 - 1;
#endif

  /* We initialize DCT_scaled_size and min_DCT_scaled_size to DCTSIZE in lossy
   * mode.  In the full decompressor, this will be overridden by jdmaster.c;
   * but in the transcoder, jdmaster.c is not used, so we must do it here.
   */
#if JPEG_LIB_VERSION >= 70
  cinfo->min_DCT_h_scaled_size = cinfo->min_DCT_v_scaled_size = data_unit;
#else
  cinfo->min_DCT_scaled_size = data_unit;
#endif

  /* Compute dimensions of components */
  for (ci = 0, compptr = cinfo->comp_info; ci < cinfo->num_components;
       ci++, compptr++) {
#if JPEG_LIB_VERSION >= 70
    compptr->DCT_h_scaled_size = compptr->DCT_v_scaled_size = data_unit;
#else
    compptr->DCT_scaled_size = data_unit;
#endif
    /* Size in data units */
    compptr->width_in_blocks = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_width * (long)compptr->h_samp_factor,
                    (long)(cinfo->max_h_samp_factor * data_unit));
    compptr->height_in_blocks = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_height * (long)compptr->v_samp_factor,
                    (long)(cinfo->max_v_samp_factor * data_unit));
    /* Set the first and last MCU columns to decompress from multi-scan images.
     * By default, decompress all of the MCU columns.
     */
    cinfo->master->first_MCU_col[ci] = 0;
    cinfo->master->last_MCU_col[ci] = compptr->width_in_blocks - 1;
    /* downsampled_width and downsampled_height will also be overridden by
     * jdmaster.c if we are doing full decompression.  The transcoder library
     * doesn't use these values, but the calling application might.
     */
    /* Size in samples */
    compptr->downsampled_width = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_width * (long)compptr->h_samp_factor,
                    (long)cinfo->max_h_samp_factor);
    compptr->downsampled_height = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_height * (long)compptr->v_samp_factor,
                    (long)cinfo->max_v_samp_factor);
    /* Mark component needed, until color conversion says otherwise */
    compptr->component_needed = TRUE;
    /* Mark no quantization table yet saved for component */
    compptr->quant_table = NULL;
  }

  /* Compute number of fully interleaved MCU rows. */
  cinfo->total_iMCU_rows = (JDIMENSION)
    jdiv_round_up((long)cinfo->image_height,
                  (long)(cinfo->max_v_samp_factor * data_unit));

  /* Decide whether file contains multiple scans */
  if (cinfo->comps_in_scan < cinfo->num_components || cinfo->progressive_mode)
    cinfo->inputctl->has_multiple_scans = TRUE;
  else
    cinfo->inputctl->has_multiple_scans = FALSE;
}


LOCAL(void)
per_scan_setup(j_decompress_ptr cinfo)
/* Do computations that are needed before processing a JPEG scan */
/* cinfo->comps_in_scan and cinfo->cur_comp_info[] were set from SOS marker */
{
  int ci, mcublks, tmp;
  jpeg_component_info *compptr;
  int data_unit = cinfo->master->lossless ? 1 : DCTSIZE;

  if (cinfo->comps_in_scan == 1) {

    /* Noninterleaved (single-component) scan */
    compptr = cinfo->cur_comp_info[0];

    /* Overall image size in MCUs */
    cinfo->MCUs_per_row = compptr->width_in_blocks;
    cinfo->MCU_rows_in_scan = compptr->height_in_blocks;

    /* For noninterleaved scan, always one data unit per MCU */
    compptr->MCU_width = 1;
    compptr->MCU_height = 1;
    compptr->MCU_blocks = 1;
    compptr->MCU_sample_width = compptr->_DCT_scaled_size;
    compptr->last_col_width = 1;
    /* For noninterleaved scans, it is convenient to define last_row_height
     * as the number of data unit rows present in the last iMCU row.
     */
    tmp = (int)(compptr->height_in_blocks % compptr->v_samp_factor);
    if (tmp == 0) tmp = compptr->v_samp_factor;
    compptr->last_row_height = tmp;

    /* Prepare array describing MCU composition */
    cinfo->blocks_in_MCU = 1;
    cinfo->MCU_membership[0] = 0;

  } else {

    /* Interleaved (multi-component) scan */
    if (cinfo->comps_in_scan <= 0 || cinfo->comps_in_scan > MAX_COMPS_IN_SCAN)
      ERREXIT2(cinfo, JERR_COMPONENT_COUNT, cinfo->comps_in_scan,
               MAX_COMPS_IN_SCAN);

    /* Overall image size in MCUs */
    cinfo->MCUs_per_row = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_width,
                    (long)(cinfo->max_h_samp_factor * data_unit));
    cinfo->MCU_rows_in_scan = (JDIMENSION)
      jdiv_round_up((long)cinfo->image_height,
                    (long)(cinfo->max_v_samp_factor * data_unit));

    cinfo->blocks_in_MCU = 0;

    for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
      compptr = cinfo->cur_comp_info[ci];
      /* Sampling factors give # of data units of component in each MCU */
      compptr->MCU_width = compptr->h_samp_factor;
      compptr->MCU_height = compptr->v_samp_factor;
      compptr->MCU_blocks = compptr->MCU_width * compptr->MCU_height;
      compptr->MCU_sample_width = compptr->MCU_width *
                                  compptr->_DCT_scaled_size;
      /* Figure number of non-dummy data units in last MCU column & row */
      tmp = (int)(compptr->width_in_blocks % compptr->MCU_width);
      if (tmp == 0) tmp = compptr->MCU_width;
      compptr->last_col_width = tmp;
      tmp = (int)(compptr->height_in_blocks % compptr->MCU_height);
      if (tmp == 0) tmp = compptr->MCU_height;
      compptr->last_row_height = tmp;
      /* Prepare array describing MCU composition */
      mcublks = compptr->MCU_blocks;
      if (cinfo->blocks_in_MCU + mcublks > D_MAX_BLOCKS_IN_MCU)
        ERREXIT(cinfo, JERR_BAD_MCU_SIZE);
      while (mcublks-- > 0) {
        cinfo->MCU_membership[cinfo->blocks_in_MCU++] = ci;
      }
    }

  }
}


/*
 * Save away a copy of the Q-table referenced by each component present
 * in the current scan, unless already saved during a prior scan.
 *
 * In a multiple-scan JPEG file, the encoder could assign different components
 * the same Q-table slot number, but change table definitions between scans
 * so that each component uses a different Q-table.  (The IJG encoder is not
 * currently capable of doing this, but other encoders might.)  Since we want
 * to be able to dequantize all the components at the end of the file, this
 * means that we have to save away the table actually used for each component.
 * We do this by copying the table at the start of the first scan containing
 * the component.
 * Rec. ITU-T T.81 | ISO/IEC 10918-1 prohibits the encoder from changing the
 * contents of a Q-table slot between scans of a component using that slot.  If
 * the encoder does so anyway, this decoder will simply use the Q-table values
 * that were current at the start of the first scan for the component.
 *
 * The decompressor output side looks only at the saved quant tables,
 * not at the current Q-table slots.
 */

LOCAL(void)
latch_quant_tables(j_decompress_ptr cinfo)
{
  int ci, qtblno;
  jpeg_component_info *compptr;
  JQUANT_TBL *qtbl;

  for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
    compptr = cinfo->cur_comp_info[ci];
    /* No work if we already saved Q-table for this component */
    if (compptr->quant_table != NULL)
      continue;
    /* Make sure specified quantization table is present */
    qtblno = compptr->quant_tbl_no;
    if (qtblno < 0 || qtblno >= NUM_QUANT_TBLS ||
        cinfo->quant_tbl_ptrs[qtblno] == NULL)
      ERREXIT1(cinfo, JERR_NO_QUANT_TABLE, qtblno);
    /* OK, save away the quantization table */
    qtbl = (JQUANT_TBL *)
      (*cinfo->mem->alloc_small) ((j_common_ptr)cinfo, JPOOL_IMAGE,
                                  sizeof(JQUANT_TBL));
    memcpy(qtbl, cinfo->quant_tbl_ptrs[qtblno], sizeof(JQUANT_TBL));
    compptr->quant_table = qtbl;
  }
}


/*
 * Initialize the input modules to read a scan of compressed data.
 * The first call to this is done by jdmaster.c after initializing
 * the entire decompressor (during jpeg_start_decompress).
 * Subsequent calls come from consume_markers, below.
 */

METHODDEF(void)
start_input_pass(j_decompress_ptr cinfo)
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;

  per_scan_setup(cinfo);
  if (!cinfo->master->lossless)
    latch_quant_tables(cinfo);
  (*cinfo->entropy->start_pass) (cinfo);
  (*cinfo->coef->start_input_pass) (cinfo);
  cinfo->inputctl->consume_input = cinfo->coef->consume_data;
  inputctl->in_scan = TRUE;
}


/*
 * Finish up after inputting a compressed-data scan.
 * This is called by the coefficient or difference controller after it's read
 * all the expected data of the scan.
 */

METHODDEF(void)
finish_input_pass(j_decompress_ptr cinfo)
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;

  inputctl->in_scan = FALSE;
  cinfo->inputctl->consume_input = consume_markers;
}


/*
 * Scan-level error recovery for progressive multi-scan files.
 *
 * j_safe_consume_input() wraps the input controller's consume_input method
 * with a setjmp() recovery point.  If a fatal error occurs while reading a
 * scan or an inter-scan marker segment of a progressive file, the partially
 * decoded scan is discarded and decoding resumes at the next marker, so a
 * single corrupt scan costs only that scan instead of the whole image.
 */

METHODDEF(void)
scan_error_exit(j_common_ptr cinfo)
/* Replacement error_exit that is in effect only while input is being
 * consumed under the protection of j_safe_consume_input().  It reports the
 * error and then longjmps back to the recovery point instead of aborting
 * decompression.
 */
{
  j_decompress_ptr dinfo = (j_decompress_ptr)cinfo;
  my_inputctl_ptr inputctl = (my_inputctl_ptr)dinfo->inputctl;

  /* Display the error message, then bail out to the recovery point. */
  (*cinfo->err->output_message)(cinfo);
  longjmp(inputctl->scan_err_jmp, 1);
}


LOCAL(boolean)
resync_to_marker(j_decompress_ptr cinfo)
/* Skip over the remainder of a corrupt data segment until a marker is
 * found, and leave that marker in cinfo->unread_marker.  Returns FALSE if
 * the data source suspends, in which case recovery is impossible.
 */
{
  int c;

  for (;;) {
    if (cinfo->src->bytes_in_buffer == 0) {
      if (!(*cinfo->src->fill_input_buffer)(cinfo))
        return FALSE;           /* suspending source: can't recover */
    }
    cinfo->src->bytes_in_buffer--;
    c = *cinfo->src->next_input_byte++;
    if (c != 0xFF)
      continue;                 /* skip non-marker byte */
    /* Skip any fill bytes and examine the marker code. */
    do {
      if (cinfo->src->bytes_in_buffer == 0) {
        if (!(*cinfo->src->fill_input_buffer)(cinfo))
          return FALSE;
      }
      cinfo->src->bytes_in_buffer--;
      c = *cinfo->src->next_input_byte++;
    } while (c == 0xFF);
    if (c == 0x00)
      continue;                 /* stuffed 0xFF data byte */
    if (c >= 0xD0 && c <= 0xD7)
      continue;                 /* stray restart marker: keep looking */
    cinfo->unread_marker = c;   /* found the next real marker */
    return TRUE;
  }
}


LOCAL(void)
zero_scan_coefficients(j_decompress_ptr cinfo)
/* Zero out the coefficients that a failed first (non-refinement) scan would
 * have contributed, so that the scan is effectively dropped rather than
 * partially applied.
 */
{
  int ci, coef;
  JDIMENSION blk_x, blk_y;
  jpeg_component_info *compptr;
  JBLOCKARRAY buffer;

  if (cinfo->coef == NULL || cinfo->Ss > cinfo->Se ||
      cinfo->Se >= DCTSIZE2 || cinfo->comps_in_scan < 1)
    return;

  for (ci = 0; ci < cinfo->comps_in_scan; ci++) {
    compptr = cinfo->cur_comp_info[ci];
    if (compptr == NULL)
      continue;
    for (blk_y = 0; blk_y < compptr->height_in_blocks; blk_y++) {
      buffer = (*cinfo->mem->access_virt_barray)
        ((j_common_ptr)cinfo,
         cinfo->coef->coef_arrays[compptr->component_index],
         blk_y, (JDIMENSION)1, TRUE);
      for (blk_x = 0; blk_x < compptr->width_in_blocks; blk_x++)
        for (coef = cinfo->Ss; coef <= cinfo->Se; coef++)
          buffer[0][blk_x][coef] = 0;
    }
  }
}


LOCAL(void)
recover_from_scan_error(j_decompress_ptr cinfo)
/* Invoked from j_safe_consume_input() when a fatal error was trapped while
 * reading a progressive scan.  Drop the partial scan and resynchronize the
 * datastream at the next marker.
 */
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;

  /* Report the lost scan.  Bypass emit_message() so that the message is
   * shown even if a warning was already emitted, since losing a scan is
   * more significant than an ordinary corrupt-data warning.
   */
  cinfo->err->msg_code = JWRN_SKIP_SCAN;
  cinfo->err->msg_parm.i[0] = cinfo->input_scan_number;
  (*cinfo->err->output_message)((j_common_ptr)cinfo);
  cinfo->err->num_warnings++;

  /* If the error occurred in the middle of a first (non-refinement) scan,
   * discard the coefficients it partially decoded.  (A refinement scan only
   * flips low-order coefficient bits, so its partial data is harmless.)
   */
  if (inputctl->in_scan && cinfo->Ah == 0)
    zero_scan_coefficients(cinfo);
  inputctl->in_scan = FALSE;

  /* Skip the remainder of the corrupt segment and resume at the next
   * marker.  unread_marker may still reference the offending marker
   * segment, so discard it first.
   */
  cinfo->unread_marker = 0;
  if (!resync_to_marker(cinfo)) {
    /* Can't recover with a suspending data source: re-throw the error. */
    (*inputctl->saved_error_exit)((j_common_ptr)cinfo);
  }
  inputctl->pub.consume_input = consume_markers;
}


GLOBAL(int)
j_safe_consume_input(j_decompress_ptr cinfo)
/* Wrapper around the input controller's consume_input method that recovers
 * from corrupt scans in progressive files: a damaged scan is discarded and
 * decoding resumes at the next scan, instead of failing the whole
 * decompression.
 */
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;
  int retcode;

  /* Recovery is only possible once we have a valid SOF and first SOS. */
  if (!cinfo->progressive_mode || inputctl->inheaders)
    return (*cinfo->inputctl->consume_input)(cinfo);

  inputctl->saved_error_exit = cinfo->err->error_exit;
  cinfo->err->error_exit = scan_error_exit;
  if (setjmp(inputctl->scan_err_jmp)) {
    /* Trapped a fatal error: recover, then tell the caller to keep going. */
    cinfo->err->error_exit = inputctl->saved_error_exit;
    recover_from_scan_error(cinfo);
    return JPEG_REACHED_SOS;
  }
  retcode = (*cinfo->inputctl->consume_input)(cinfo);
  cinfo->err->error_exit = inputctl->saved_error_exit;
  return retcode;
}


/*
 * Read JPEG markers before, between, or after compressed-data scans.
 * Change state as necessary when a new scan is reached.
 * Return value is JPEG_SUSPENDED, JPEG_REACHED_SOS, or JPEG_REACHED_EOI.
 *
 * The consume_input method pointer points either here or to the
 * coefficient or difference controller's consume_data routine, depending on
 * whether we are reading a compressed data segment or inter-segment markers.
 */

METHODDEF(int)
consume_markers(j_decompress_ptr cinfo)
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;
  int val;

  if (inputctl->pub.eoi_reached) /* After hitting EOI, read no further */
    return JPEG_REACHED_EOI;

  val = (*cinfo->marker->read_markers) (cinfo);

  switch (val) {
  case JPEG_REACHED_SOS:        /* Found SOS */
    if (inputctl->inheaders) {  /* 1st SOS */
      initial_setup(cinfo);
      inputctl->inheaders = FALSE;
      /* Note: start_input_pass must be called by jdmaster.c
       * before any more input can be consumed.  jdapimin.c is
       * responsible for enforcing this sequencing.
       */
    } else {                    /* 2nd or later SOS marker */
      if (!inputctl->pub.has_multiple_scans)
        ERREXIT(cinfo, JERR_EOI_EXPECTED); /* Oops, I wasn't expecting this! */
      start_input_pass(cinfo);
    }
    break;
  case JPEG_REACHED_EOI:        /* Found EOI */
    inputctl->pub.eoi_reached = TRUE;
    if (inputctl->inheaders) {  /* Tables-only datastream, apparently */
      if (cinfo->marker->saw_SOF)
        ERREXIT(cinfo, JERR_SOF_NO_SOS);
    } else {
      /* Prevent infinite loop in coef ctlr's decompress_data routine
       * if user set output_scan_number larger than number of scans.
       */
      if (cinfo->output_scan_number > cinfo->input_scan_number)
        cinfo->output_scan_number = cinfo->input_scan_number;
    }
    break;
  case JPEG_SUSPENDED:
    break;
  }

  return val;
}


/*
 * Reset state to begin a fresh datastream.
 */

METHODDEF(void)
reset_input_controller(j_decompress_ptr cinfo)
{
  my_inputctl_ptr inputctl = (my_inputctl_ptr)cinfo->inputctl;

  inputctl->pub.consume_input = consume_markers;
  inputctl->pub.has_multiple_scans = FALSE; /* "unknown" would be better */
  inputctl->pub.eoi_reached = FALSE;
  inputctl->inheaders = TRUE;
  inputctl->in_scan = FALSE;
  /* Reset other modules */
  (*cinfo->err->reset_error_mgr) ((j_common_ptr)cinfo);
  (*cinfo->marker->reset_marker_reader) (cinfo);
  /* Reset progression state -- would be cleaner if entropy decoder did this */
  cinfo->coef_bits = NULL;
}


/*
 * Initialize the input controller module.
 * This is called only once, when the decompression object is created.
 */

GLOBAL(void)
jinit_input_controller(j_decompress_ptr cinfo)
{
  my_inputctl_ptr inputctl;

  /* Create subobject in permanent pool */
  inputctl = (my_inputctl_ptr)
    (*cinfo->mem->alloc_small) ((j_common_ptr)cinfo, JPOOL_PERMANENT,
                                sizeof(my_input_controller));
  cinfo->inputctl = (struct jpeg_input_controller *)inputctl;
  /* Initialize method pointers */
  inputctl->pub.consume_input = consume_markers;
  inputctl->pub.reset_input_controller = reset_input_controller;
  inputctl->pub.start_input_pass = start_input_pass;
  inputctl->pub.finish_input_pass = finish_input_pass;
  /* Initialize state: can't use reset_input_controller since we don't
   * want to try to reset other modules yet.
   */
  inputctl->pub.has_multiple_scans = FALSE; /* "unknown" would be better */
  inputctl->pub.eoi_reached = FALSE;
  inputctl->inheaders = TRUE;
  inputctl->in_scan = FALSE;
}
