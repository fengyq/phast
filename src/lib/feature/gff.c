/***************************************************************************
 * PHAST: PHylogenetic Analysis with Space/Time models
 * Copyright (c) 2002-2005 University of California, 2006-2009 Cornell 
 * University.  All rights reserved.
 *
 * This source code is distributed under a BSD-style license.  See the
 * file LICENSE.txt for details.
 ***************************************************************************/

/* $Id: gff.c,v 1.37 2008-11-12 02:07:59 acs Exp $ */

/** \file gff.c
    Reading and writing of sequence features in General Feature Format
    (GFF).  Obeys file specification at
    http://www.sanger.ac.uk/Software/formats/GFF/GFF_Spec.shtml. 
    \ingroup feature
*/
   
#include <gff.h>
#include <assert.h>
#include <string.h>
#include <time.h>
#include <hashtable.h>
#include <misc.h>
#include <ctype.h>
#include <bed.h>
#include <genepred.h>

/** Read a set of features from a file and return a newly allocated
   GFF_Set object.  Function reads until end-of-file is encountered or
   error occurs (aborts on error).  Comments and blank lines are
   ignored and special "meta-data" comments are parsed (see
   http://www.sanger.ac.uk/resources/software/gff/spec.html).  Only
   the first five columns of feature lines are considered required
   ('name', 'source', 'feature', 'start', and 'end'); subsequent
   fields are optional ('score', 'strand', 'frame', and 'attribute').
   Default value for score, strand, and frame is null ('.') and for
   attribute is the empty string ('').  Columns must be separated by
   tabs.  */
GFF_Set* gff_read_set(FILE *F) {
  int start, end, frame, score_is_null, lineno, done_with_header = FALSE,
    seekable = TRUE;
  double score;
  char strand;
  String *attr, *line;
  GFF_Feature *feat;
  GFF_Set *set;
  List *l, *substrs;
  static Regex *spec_comment_re = NULL;
  fpos_t pos;

  line = str_new(STR_LONG_LEN);
  set = gff_new_set();
  l = lst_new_ptr(GFF_NCOLS);
  substrs = lst_new_ptr(4);

  /* mark start position; used for autodetection of bed and genepred formats */
  if (fgetpos(F, &pos) != 0) seekable = FALSE;

  lineno = 0;
  while (str_readline(line, F) != EOF) {
    lineno++;

    str_double_trim(line);
    if (line->length == 0) continue;

    if (!done_with_header && str_starts_with_charstr(line, "##")) {
      int unrecognized = 0;
      if (spec_comment_re == NULL)
        spec_comment_re = str_re_new("^[[:space:]]*##[[:space:]]*([^[:space:]]+)[[:space:]]+([^[:space:]]+)([[:space:]]+([^[:space:]]+))?");
 
      if (str_re_match(line, spec_comment_re, substrs, 4) >= 0) {
        String *tag, *val1, *val2;
        tag = (String*)lst_get_ptr(substrs, 1);
        val1 = (String*)lst_get_ptr(substrs, 2);
        val2 =  lst_size(substrs) > 4 ? (String*)lst_get_ptr(substrs, 4) : NULL;
        
        if (str_equals_nocase_charstr(tag, GFF_VERSION_TAG))
          str_cpy(set->gff_version, val1);
        else if (str_equals_nocase_charstr(tag, GFF_SOURCE_VERSION_TAG) && 
                 val2 != NULL) {
          str_cpy(set->source, val1);
          str_cpy(set->source_version, val2); 
        }
        else if (str_equals_nocase_charstr(tag, GFF_DATE_TAG))
          str_cpy(set->date, val1);
        else unrecognized = 1;
      }
      else unrecognized = 1;

/*       if (unrecognized) */
/*         fprintf(stderr, "WARNING: unrecognized meta-data: '%s'\n",  */
/*                 line->chars); */
      lst_free_strings(substrs);

      continue;
    }

    else if (line->chars[0] == '#') continue; /* just skip ordinary comments */
    
    done_with_header = 1;

    str_split(line, "\t", l);

    /* if first record, check to see if the file's a BED or a
       genepred.  If there are 3-8 or 12 columns, and if the 2nd and
       3rd columns are integers, then we'll try reading it as a BED.
       If >=10 columns and cols 4-7 are integers, we'll try reading it
       as a genepred */
    if (lst_size(set->features) == 0) {
      if (((lst_size(l) >= 3 && lst_size(l) <= 8) || lst_size(l) == 12) && 
          str_as_int(lst_get_ptr(l, 1), &start) == 0 && 
          str_as_int(lst_get_ptr(l, 2), &end) == 0) {
        if (!seekable) 
          die("ERROR: Looks like BED format but can't rewind (non-seekable stream).\n");
        fsetpos(F, &pos);
        gff_read_from_bed(set, F);
        break;
      }
      else if (lst_size(l) >= 10 && 
          str_as_int(lst_get_ptr(l, 3), &start) == 0 && 
          str_as_int(lst_get_ptr(l, 4), &end) == 0 &&
          str_as_int(lst_get_ptr(l, 5), &start) == 0 &&
          str_as_int(lst_get_ptr(l, 6), &end) == 0) {
        if (!seekable) 
          die("ERROR: Looks like genepred format but can't rewind (non-seekable stream).\n");
        fsetpos(F, &pos);
        gff_read_from_genepred(set, F);
        break;
      }
    }

    /* set defaults for optional fields */
    strand = '.';
    frame = GFF_NULL_FRAME;
    score_is_null = 1;            

    if (lst_size(l) < GFF_MIN_NCOLS) 
      die("ERROR at line %d (gff_read_set): minimum of %d columns are required.\n", 
          lineno, GFF_MIN_NCOLS);
  
    if (str_as_int(lst_get_ptr(l, 3), &start) != 0) 
      die("ERROR at line %d (gff_read_set): non-numeric 'start' value ('%s').\n", 
          lineno, ((String*)lst_get_ptr(l, 3))->chars);

    if (str_as_int((String*)lst_get_ptr(l, 4), &end) != 0) 
      die("ERROR at line %d (gff_read_set): non-numeric 'end' value ('%s').\n", 
          lineno, ((String*)lst_get_ptr(l, 4))->chars);

    if (lst_size(l) > 5) {
      String *score_str = (String*)lst_get_ptr(l, 5);
      if (! str_equals_charstr(score_str, ".")) {
        if (str_as_dbl(score_str, &score) != 0) 
          die( "ERROR at line %d (gff_read_set): non-numeric and non-null 'score' value ('%s').\n", 
                  lineno, score_str->chars);
        else 
          score_is_null = 0;
      }
    }

    strand = '.';
    if (lst_size(l) > 6) {
      String *tmp = (String*)lst_get_ptr(l, 6);
      if (tmp->length == 0 || tmp->length > 1 || 
          (tmp->chars[0] != '+' && tmp->chars[0] != '-' && 
           tmp->chars[0] != '.')) 
        die("ERROR at line %d: illegal 'strand' ('%s').\n", 
            lineno, tmp->chars);
      strand = tmp->chars[0];
    }

    frame = GFF_NULL_FRAME;
    if (lst_size(l) > 7) {
      String *tmp = (String*)lst_get_ptr(l, 7);
      if (! str_equals_charstr(tmp, ".") != 0) {
        if (str_as_int(tmp, &frame) != 0 || frame < 0 || frame > 2) 
          die("ERROR at line %d: illegal 'frame' ('%s').\n", 
              lineno, tmp->chars);
        frame = (3 - frame) % 3; /* convert to internal
                                    representation */
      }
    }

    if (lst_size(l) > 8) 
      attr = str_dup(lst_get_ptr(l, 8));
    else attr = str_new(0);
    
    feat = gff_new_feature(str_dup(lst_get_ptr(l, 0)), 
                           str_dup(lst_get_ptr(l, 1)),
                           str_dup(lst_get_ptr(l, 2)), start, end, score, 
                           strand, frame, attr, score_is_null);

    lst_push_ptr(set->features, feat);
    lst_free_strings(l);
  }

  str_free(line);
  lst_free(l);
  lst_free(substrs);
  return set;
}

/** Create new GFF_Feature object with specified attributes.  Strings
   are copied by reference.  Returns newly allocated GFF_Feature
   object. */
GFF_Feature *gff_new_feature(String *seqname, String *source, String *feature,
                             int start, int end, double score, char strand, 
                             int frame, String *attribute, 
                             int score_is_null) {
  GFF_Feature *feat = (GFF_Feature*)smalloc(sizeof(GFF_Feature));

  assert(seqname != NULL && source != NULL && feature != NULL && 
         attribute != NULL && 
         (strand == '+' || strand == '-' || strand == '.') &&
         (frame == GFF_NULL_FRAME || (0 <= frame && frame <=2)));

  feat->seqname = seqname;
  feat->source = source;
  feat->feature = feature;
  feat->start = start;
  feat->end = end;
  feat->score = score;
  feat->strand = strand;
  feat->frame = frame;
  feat->attribute = attribute;
  feat->score_is_null = score_is_null;
  return feat;
}

/** Create a new GFF_Feature from a genomic position string of the
   type used in the UCSC genome browser, e.g.,
   chr10:102553847-102554897.  A trailing '+' or '-' will be
   interpreted as the strand; otherwise the null strand is used.  NULL
   is returned if the string can't be parsed. */
GFF_Feature *gff_new_feature_genomic_pos(String *position, String *source, 
                                         String *feature, double score, 
                                         int frame, String *attribute,
                                         int score_is_null) {
  GFF_Feature *retval = NULL;
  List *substrs = lst_new_ptr(4);
  static Regex *posre = NULL;
  if (posre == NULL) 
    posre = str_re_new("(chr[_a-zA-Z0-9]+):([0-9]+)-([0-9]+)([-+])?");

  if (str_re_match(position, posre, substrs, 4) >= 3) {
    int start, end;
    char strand= '.';
    String *chr = str_dup(lst_get_ptr(substrs, 1)), 
      *tmpstr = lst_get_ptr(substrs, 4);
    str_as_int(lst_get_ptr(substrs, 2), &start);
    str_as_int(lst_get_ptr(substrs, 3), &end);
    if (tmpstr != NULL) strand = tmpstr->chars[0];
    return gff_new_feature(chr, source, feature, start, end, score, 
                           strand, frame, attribute, score_is_null);
  }
  lst_free_strings(substrs);
  lst_free(substrs);
  return retval;
}


/** Create new GFF_Set object with the number of rows specified */
GFF_Set *gff_new_set_len(int len) {
  GFF_Set *set = (GFF_Set*)smalloc(sizeof(GFF_Set));
  set->features = lst_new_ptr(len);
  set->gff_version = str_new(STR_SHORT_LEN);
  set->source = str_new(STR_SHORT_LEN);
  set->source_version = str_new(STR_SHORT_LEN);
  set->date = str_new(STR_SHORT_LEN);
  set->groups = NULL;
  set->group_tag = NULL;
  return set;
}


/** Create new GFF_Set object.  All attributes will be left as empty
    strings.  */
GFF_Set *gff_new_set() {
  return (GFF_Set*)gff_new_set_len(GFF_SET_START_SIZE);
}


/** Create new GFF_Set object, initializing with same version, source,
    etc.\ as a template GFF_Set object */
GFF_Set *gff_new_from_template(GFF_Set *gff) {
  GFF_Set *retval = gff_new_set();
  str_cpy(retval->gff_version, gff->gff_version);
  str_cpy(retval->source, gff->source);
  str_cpy(retval->source_version, gff->source_version);
  str_cpy(retval->date, gff->date); 
  return retval;
}

/** Create new GFF_Set object, using typical defaults and other
   parameters as specified.  Sets gff version to '2' and date to current
   date, and sets source and source version as specified. */
GFF_Set *gff_new_set_init(char *source, char *source_version) {
  struct tm *tm;
  time_t t;
  GFF_Set *set = gff_new_set();
  str_append_int(set->gff_version, GFF_DEFAULT_VERSION);
  str_cpy_charstr(set->source, source);
  str_cpy_charstr(set->source_version, source_version);

  t = time(NULL);
  tm = localtime(&t);
  str_append_int(set->date, 1900 + tm->tm_year);
  str_append_charstr(set->date, "-");
  str_append_int(set->date, tm->tm_mon + 1);
  str_append_charstr(set->date, "-");
  str_append_int(set->date, tm->tm_mday);

  return set;
}

/** Free resources associated with GFF_Set object (including all
   features and the set object itself). */ 
void gff_free_set(GFF_Set *set) {
  int i;
  if (set->features != NULL) {
    for (i = 0; i < lst_size(set->features); i++)
      gff_free_feature((GFF_Feature*)lst_get_ptr(set->features, i));
    lst_free(set->features);
  }
  str_free(set->gff_version);
  str_free(set->source);
  str_free(set->source_version);
  str_free(set->date);
  if (set->groups != NULL) gff_ungroup(set);
  free(set);
}

/** Free resources associated with GFF_Feature object.  */
void gff_free_feature(GFF_Feature *feat) {
  str_free(feat->seqname);
  str_free(feat->source);
  str_free(feat->feature);
  str_free(feat->attribute);
  free(feat);
}

/** Output a GFF_Set to the specified stream in GFF. */
void gff_print_set(FILE *F, GFF_Set *set) {
  int i;

  if (set->gff_version->length > 0)
    fprintf(F, "##%s %s\n", GFF_VERSION_TAG, set->gff_version->chars);

  if (set->source_version->length > 0)
    fprintf(F, "##%s %s %s\n", GFF_SOURCE_VERSION_TAG, set->source->chars, 
            set->source_version->chars);

  if (set->date->length > 0)
    fprintf(F, "##%s %s\n", GFF_DATE_TAG, set->date->chars);

  if (set->features != NULL)
    for (i = 0; i < lst_size(set->features); i++) 
      gff_print_feat(F, (GFF_Feature*)lst_get_ptr(set->features, i));
}

/** Print an individual GFF_Feature object as a GFF line. */
void gff_print_feat(FILE *F, GFF_Feature *feat) {
  char score_str[50], frame_str[50];

  if (feat->score_is_null) strcpy(score_str, ".");
  else sprintf(score_str, "%.3f", feat->score);

  if (feat->frame == GFF_NULL_FRAME) strcpy(frame_str, ".");
  else sprintf(frame_str, "%d", (3 - feat->frame) % 3);
                                /* NOTE: have to convert from internal
                                   representation to GFF
                                   representation */

  fprintf(F, "%s\t%s\t%s\t%d\t%d\t%s\t%c\t%s\t%s\n", 
          feat->seqname->chars, feat->source->chars, feat->feature->chars, 
          feat->start, feat->end, score_str, feat->strand, frame_str, 
          feat->attribute->chars);    
}

/** Create an exact copy of a GFF_Feature object */
GFF_Feature *gff_new_feature_copy(GFF_Feature *orig) {
  String *seqname = str_dup(orig->seqname);
  String *source = str_dup(orig->source);
  String *feature = str_dup(orig->feature);
  String *attribute = str_dup(orig->attribute);
  return gff_new_feature(seqname, source, feature, orig->start, orig->end, 
                         orig->score, orig->strand, orig->frame, attribute, 
                         orig->score_is_null);
}


/** Create a new GFF_Set representing the features in a particular
    coordinate range.  Keeps features such that feat->start >= startcol
    and feat->end <= endcol. */
GFF_Set *gff_subset_range(GFF_Set *set, int startcol, int endcol, 
                          int reset_indices) {
  GFF_Set *subset = gff_new_set();
  int i;

  str_cpy(subset->gff_version, set->gff_version);
  str_cpy(subset->source, set->source);
  str_cpy(subset->source_version, set->source_version);
  str_cpy(subset->date, set->date); /* make current date instead? */

  /* Note: uses linear search */
  for (i = 0; i < lst_size(set->features); i++) {
    GFF_Feature *feat = (GFF_Feature*)lst_get_ptr(set->features, i);
    if (feat->start >= startcol && feat->end <= endcol) {
      GFF_Feature *newfeat = gff_new_feature_copy(feat);
      if (reset_indices) {
        newfeat->start = newfeat->start - startcol + 1;
        newfeat->end = newfeat->end - startcol + 1;
      }
      lst_push_ptr(subset->features, newfeat);
    }
  }
  return subset;
}

/** Like gff_subset_range, except keep any featuers that
    overlap with range (even if parts of the feature fall outside 
    range) **/
GFF_Set *gff_subset_range_overlap(GFF_Set *set, int startcol, int endcol) {
  GFF_Set *subset = NULL;
  int i;

  /* Note: uses linear search */
  for (i = 0; i < lst_size(set->features); i++) {
    GFF_Feature *feat = (GFF_Feature*)lst_get_ptr(set->features, i);
    if (feat->start <= endcol && feat->end >= startcol) {
      GFF_Feature *newfeat = gff_new_feature_copy(feat);
      if (subset == NULL) {
	subset = gff_new_set();
	str_cpy(subset->gff_version, set->gff_version);
	str_cpy(subset->source, set->source);
	str_cpy(subset->source_version, set->source_version);
	str_cpy(subset->date, set->date); /* make current date instead? */
      }
      lst_push_ptr(subset->features, newfeat);
    }
  }
  return subset;
}


/** Like gff_subset_range_overlap, but assume gff is sorted by start
    position of feature.  Start search at index startSearchIdx and assume
    that there are no overlapping features before this index.  Reset
    startSearchIdx to the first matching feature (or leave it alone if no
    matches).  Stop searching gff when indices in gff exceed endcol */
GFF_Set *gff_subset_range_overlap_sorted(GFF_Set *set, int startcol, int endcol,
					 int *startSearchIdx) {
  GFF_Set *subset = NULL;
  int i;

  /* Note: uses linear search */
  for (i = *startSearchIdx; i < lst_size(set->features); i++) {
    GFF_Feature *feat = (GFF_Feature*)lst_get_ptr(set->features, i);
    if (feat->start <= endcol && feat->end >= startcol) {
      GFF_Feature *newfeat = gff_new_feature_copy(feat);
      if (subset == NULL) {
	subset = gff_new_set();
	str_cpy(subset->gff_version, set->gff_version);
	str_cpy(subset->source, set->source);
	str_cpy(subset->source_version, set->source_version);
	str_cpy(subset->date, set->date); /* make current date instead? */
	*startSearchIdx = i;
      }
      lst_push_ptr(subset->features, newfeat);
    }
    else if (feat->start > endcol) break;
  }
  return subset;
}

/** Discard any feature whose feature type is not in the specified
    list. */
void gff_filter_by_type(GFF_Set *gff, 
                                /**< GFF_Set to process */
                        List *types, 
                                /**< Feature types to include (List of
                                   Strings) */
                        int exclude,
                                /**< Exclude rather than include
                                   specified types */
                        FILE *discards_f
                                /**< Discarded features will be
                                   written here (if non-NULL) */
                        ) {
  List *newfeats = lst_new_ptr(lst_size(gff->features));
  int i, changed = FALSE;
  for (i = 0; i < lst_size(gff->features); i++) {
    GFF_Feature *f = lst_get_ptr(gff->features, i);
    int in_list = str_in_list(f->feature, types);

    if ((in_list == TRUE && exclude == FALSE) || 
        (in_list == FALSE && exclude == TRUE))
      lst_push_ptr(newfeats, f);
    else {
      if (discards_f != NULL) gff_print_feat(discards_f, f);
      gff_free_feature(f);
      changed = TRUE;
    }
  }
  lst_free(gff->features);
  gff->features = newfeats;
  if (changed && gff->groups != NULL)
    gff_ungroup(gff);
}

/** Test whether a set of GFF_Feature objects refers to the reverse
   strand.  Returns 1 if no features have strand equal to '+' and at
   least one has strand equal to '-'; otherwise returns 0. */
int gff_reverse_strand_only(List *features) {
  int i, impossible = 0, possible = 0;
  for (i = 0; !impossible && i < lst_size(features); i++) {
    GFF_Feature *feat = (GFF_Feature*)lst_get_ptr(features, i);
    if (feat->strand == '-') 
      possible = 1;
    else if (feat->strand == '+')
      impossible = 1;
  }
  if (impossible || !possible)
    return 0;
  return 1;
}

/** Adjust coordinates and strand of GFF_Feature objects to reflect
   reverse complementation of given interval of sequence.  Also
   reverses order of appearance of features.  The features, the
   start_range, and the end_range are all assumed to use the same
   coordinate frame. */
void gff_reverse_compl(List *features,
                                /**< List of GFF_Feature objects */
                       int start_range, 
                                /**< First coordinate of interval 
                                   (inclusive, 1-based indexing, as in
                                   features) */
                       int end_range
                                /**< Last coordinate of interval
                                   (inclusive, 1-based indexing, as in
                                   features) */
                       ) {
  int i;
  for (i = 0; i < lst_size(features); i++) {
    GFF_Feature *feat = lst_get_ptr(features, i);
    int tmp = feat->start;
    feat->start = end_range - feat->end + start_range;
    feat->end = end_range - tmp + start_range;
    if (feat->strand == '-') feat->strand = '+';
    else if (feat->strand == '+') feat->strand = '-';
  }
  /* also reverse order of features (will generally be in ascending order) */
  for (i = 0; i < lst_size(features)/2; i++) {
    GFF_Feature *tmp = lst_get_ptr(features, i);
    lst_set_ptr(features, i, 
                lst_get_ptr(features, lst_size(features)-i-1));
    lst_set_ptr(features, lst_size(features)-i-1, tmp);
  }
}

/* used by gff_sort (see below) */
int gff_feature_comparator(const void* ptr1, const void* ptr2) {
  GFF_Feature *feat1 = *((GFF_Feature**)ptr1);
  GFF_Feature *feat2 = *((GFF_Feature**)ptr2);
  if (feat1->start != feat2->start) 
    return (feat1->start - feat2->start);
  return (feat1->end - feat2->end);
                                /* note that this rule has the effect
                                   of putting short features that
                                   overlap the ends of longer ones in
                                   a sensible order */
}

/* used by gff_sort (see below) */
int gff_group_comparator(const void* ptr1, const void* ptr2) {
  GFF_FeatureGroup *group1 = *((GFF_FeatureGroup**)ptr1);
  GFF_FeatureGroup *group2 = *((GFF_FeatureGroup**)ptr2);
  if (lst_size(group1->features) == 0 || 
      lst_size(group2->features) == 0) 
    return 0;                   /* just to be safe... */

  if (group1->start != group2->start) 
    return (group1->start - group2->start);
  return (group1->end - group2->end);
}

/** Sort features primarily by start position, and secondarily by end
    position (ascending order).  If features are grouped (see
    gff_group), then they will be sorted within groups, and groups
    will be sorted by start position of first feature */
void gff_sort(GFF_Set *set) {
  int i, j;
  if (set->groups == NULL)
    lst_qsort(set->features, gff_feature_comparator);
  else {
    for (i = 0; i < lst_size(set->groups); i++)
      lst_qsort(((GFF_FeatureGroup*)lst_get_ptr(set->groups, i))->features, 
                gff_feature_comparator);
    lst_qsort(set->groups, gff_group_comparator);
    /* now reorder features according to groups */
    lst_clear(set->features);
    for (i = 0; i < lst_size(set->groups); i++) {
      GFF_FeatureGroup *group = lst_get_ptr(set->groups, i);
      for (j = 0; j < lst_size(group->features); j++)
        lst_push_ptr(set->features, lst_get_ptr(group->features, j));
    }
  }
}

/** Group features by value of specified tag.  All features with
    undefined values will be placed in a single group. */
void gff_group(GFF_Set *set, char *tag) {
  char tmpstr[STR_SHORT_LEN];
  Regex *tag_re;
  List *l = lst_new_ptr(1);
  int est_no_groups = max(lst_size(set->features) / 10, 1);
  Hashtable *hash = hsh_new(est_no_groups);
  String *nullstr = str_new(1); /* empty string represents missing or
                                   null value for tag */
  int i, taglen = strlen(tag);

  if (set->groups != NULL)
    gff_ungroup(set);

  set->groups = lst_new_ptr(est_no_groups);
  set->group_tag = str_new_charstr(tag);

  /* since we only use the 'attribute' field for grouping, we'll store
     it unparsed, and parse it only when we need to group */

  sprintf(tmpstr, ".*%s[[:space:]]+(\"[^\"]*\"|[^[:space:]]+)", tag);
  tag_re = str_re_new(tmpstr);

  for (i = 0; i < lst_size(set->features); i++) {
    GFF_Feature *f = lst_get_ptr(set->features, i);
    String *val = nullstr;
    GFF_FeatureGroup *group;
    lst_clear(l);

    if (f->attribute->length > taglen && /* avoid checking empty
                                            or null attrs */
        str_re_match(f->attribute, tag_re, l, 1) >= 0) {
      val = lst_get_ptr(l, 1);
      if (str_ends_with_charstr(val, ";"))
        val->chars[--val->length] = '\0';
      str_remove_quotes(val);
    }

    if ((group = hsh_get(hash, val->chars)) == (void*)-1) {
                                /* new group */
      group = smalloc(sizeof(GFF_FeatureGroup));
      group->name = str_dup(val);
      group->features = lst_new_ptr(5);
      group->start = f->start;
      group->end = f->end;
      lst_push_ptr(set->groups, group);
      hsh_put(hash, val->chars, group);
    }
    else {                      /* new member for existing group */
      if (f->start < group->start) group->start = f->start;
      if (f->end > group->end) group->end = f->end;
    }
    lst_push_ptr(group->features, f);

    lst_free_strings(l);
  }

  lst_free(l);
  str_re_free(tag_re);
  str_free(nullstr);
  hsh_free(hash);
}


/** Group features by feature type.  All features with
    undefined values will be placed in a single group. */
void gff_group_by_feature(GFF_Set *set) {
  int est_no_groups = max(lst_size(set->features) / 10, 1);
  Hashtable *hash = hsh_new(est_no_groups);
  int i;

  if (set->groups != NULL)
    gff_ungroup(set);

  set->groups = lst_new_ptr(est_no_groups);
  set->group_tag = str_new_charstr("feature");

  for (i = 0; i < lst_size(set->features); i++) {
    GFF_Feature *f = lst_get_ptr(set->features, i);
    GFF_FeatureGroup *group;
    
    if ((group = hsh_get(hash, f->feature->chars)) == (void*)-1) {
                                /* new group */
      group = smalloc(sizeof(GFF_FeatureGroup));
      group->name = str_dup(f->feature);
      group->features = lst_new_ptr(5);
      group->start = f->start;
      group->end = f->end;
      lst_push_ptr(set->groups, group);
      hsh_put(hash, f->feature->chars, group);
    }
    else {                      /* new member for existing group */
      if (f->start < group->start) group->start = f->start;
      if (f->end > group->end) group->end = f->end;
    }
    lst_push_ptr(group->features, f);

  }
  hsh_free(hash);
}


/** Remove grouping of features */
void gff_ungroup(GFF_Set *set) {
  int i;
  if (set->groups == NULL) return;
  for (i = 0; i < lst_size(set->groups); i++) {
    GFF_FeatureGroup *group = lst_get_ptr(set->groups, i);
    str_free(group->name);
    lst_free(group->features);
    free(group);
  }
  lst_free(set->groups);
  set->groups = NULL;
  str_free(set->group_tag);
  set->group_tag = NULL;
}

/** Group contiguous features, e.g., an exon and adjacent splice
    sites.  If features have already been grouped (e.g., by transcript
    id), then subgroups are created by adding new tags. New tag values
    will be composed of the tag value for the "outer" group (e.g., the
    transcript_id) and a unique suffix indicating the "inner" group.
    In all cases, feature are sorted as a side effect, in a way that
    reflects the initial grouping, not the grouping into exons. */ 
void gff_exon_group(GFF_Set *set, /**< Set to process  */
                    char *tag   /**< Tag to use for new groups (e.g.,
                                   "exon_id")  */
                    ) {
  List *groups;
  int i, j;
  char tmpstr[STR_MED_LEN];
  GFF_FeatureGroup *dummy = NULL;

  gff_sort(set);

  if (set->groups == NULL) {
    /* create dummy list consisting of one group, to simplify code
       below */
    dummy = smalloc(sizeof(GFF_FeatureGroup));
    dummy->name = NULL;
    dummy->features = set->features;
    groups = lst_new_ptr(1);
    lst_push_ptr(groups, dummy);
  }
  else 
    groups = set->groups;

  for (i = 0; i < lst_size(groups); i++) {
    GFF_FeatureGroup *group = lst_get_ptr(groups, i);
    int idx = 0;
    GFF_Feature *lastfeat = NULL;
    for (j = 0; j < lst_size(group->features); j++) {
      GFF_Feature *f = lst_get_ptr(group->features, j);
      if (lastfeat == NULL || f->start > lastfeat->end + 1 || 
          f->strand != lastfeat->strand) 
        idx++;

      if (f->attribute->length == 0 || str_equals_charstr(f->attribute, "."))
        str_clear(f->attribute);
      else 
        str_append_charstr(f->attribute, " ; ");

      if (group->name == NULL || group->name->length == 0)
        sprintf(tmpstr, "%s \"%d\"", tag, idx);
      else 
        sprintf(tmpstr, "%s \"%s.%d\"", tag, group->name->chars, idx);
      
      str_append_charstr(f->attribute, tmpstr);

      if (lastfeat == NULL || f->end > lastfeat->end)
        lastfeat = f;
    }
  }

  /* now regroup using tag for subgroups */
  gff_group(set, tag);

  if (dummy != NULL) {
    free(dummy);
    lst_free(groups);
  }
}

/** Identify overlapping groups and remove all but the first
   one encountered.  Features must already be grouped. */
void gff_remove_overlaps(GFF_Set *gff, 
                                /**< Set to process */
                         FILE *discards_f
                                /**< If non-NULL, discarded features
                                   will be written here */
                         ) {
  int i, j, k, last_end = -1;
  List *starts, *ends, *scores, *keepers, *discards;

  if (gff->groups == NULL) 
    die("ERROR: gff_remove_overlaps requires groups.\n");

  starts = lst_new_int(lst_size(gff->groups));
  ends = lst_new_int(lst_size(gff->groups));
  scores = lst_new_dbl(lst_size(gff->groups));
  keepers = lst_new_ptr(lst_size(gff->groups));
  discards = lst_new_ptr(10);   /* handle only a few at a time */

  for (i = 0; i < lst_size(gff->groups); i++) {
    GFF_FeatureGroup *group = lst_get_ptr(gff->groups, i);
    double score = 0;
    int has_scores = FALSE;

    /* get a rough "score" to use in deciding which of a pair of
       overlapping groups to keep.  Use sum of scores of features, or
       if scores aren't available, just use span of group */
    for (j = 0; j < lst_size(group->features); j++) {
      GFF_Feature *f = lst_get_ptr(group->features, j);
      if (!f->score_is_null) { score += f->score; has_scores = TRUE; }
    }
    if (!has_scores) score = group->end - group->start + 1;

    /* check for overlap */
    if (group->start > last_end) {   /* common case, has to be safe */
      lst_push_int(starts, group->start);
      lst_push_int(ends, group->end);
      lst_push_dbl(scores, score);
      lst_push_ptr(keepers, group);
      last_end = group->end;
    }
    else {                      /* have to search list */
      int list_idx = lst_bsearch_int(starts, group->start);
                                /* indicates *previous* feature (-1 if
                                   'group' belongs at front of list)  */
      int prev_end = list_idx >= 0 ? lst_get_int(ends, list_idx) : -1;
      int next_start = list_idx+1 < lst_size(starts) ?
        lst_get_int(starts, list_idx+1) : INFTY;         
      int add_this_group = TRUE;
      lst_clear(discards);

      if (prev_end >= group->start || next_start <= group->end) {
        /* overlaps a previous group: compute total score of all
           overlapping groups; if less than score of this group,
           replace all of them with this one */
        int minidx, maxidx;
        double altscore = 0;
        for (minidx = list_idx; 
             minidx >= 0 && lst_get_int(ends, minidx) >= group->start;
             minidx--)
          altscore += lst_get_dbl(scores, minidx);
        minidx++;               /* will always go one too far */
        for (maxidx = list_idx + 1; 
             maxidx < lst_size(starts) && lst_get_int(starts, maxidx) <= group->end;
             maxidx++)
          altscore += lst_get_dbl(scores, maxidx);
        maxidx--;

        if (score > altscore) { /* discard the others and add this one */
          for (; maxidx >= minidx; maxidx--) {
            lst_delete_idx(starts, minidx);
            lst_delete_idx(ends, minidx);
            lst_delete_idx(scores, minidx);
            lst_push_ptr(discards, lst_get_ptr(keepers, minidx));
            lst_delete_idx(keepers, minidx);
          }
          add_this_group = TRUE;
          list_idx = minidx - 1;  /* needed for insert, below */
        }
        else {                  /* discard this one */
          lst_push_ptr(discards, group);
          add_this_group = FALSE;
        }
      }

      /* add group, if necessary */
      if (add_this_group) {
        lst_insert_idx_int(starts, list_idx, group->start);
        lst_insert_idx_int(ends, list_idx, group->end);
        lst_insert_idx_dbl(scores, list_idx, score);
        lst_insert_idx_ptr(keepers, list_idx, group);
        if (group->end > last_end) last_end = group->end;
      }

      /* free discarded groups and dump to file, if necessary */
      for (k = 0; k < lst_size(discards); k++) {
        GFF_FeatureGroup *g = lst_get_ptr(discards, k);
        if (discards_f != NULL)
          for (j = 0; j < lst_size(g->features); j++)
            gff_print_feat(discards_f, lst_get_ptr(g->features, j));
        for (j = 0; j < lst_size(g->features); j++)
          gff_free_feature(lst_get_ptr(g->features, j));
        lst_free(g->features);
        str_free(g->name);
      }
    }
  }

  lst_free(gff->groups);
  gff->groups = keepers;
  lst_clear(gff->features);
  for (i = 0; i < lst_size(gff->groups); i++) {
    GFF_FeatureGroup *group = lst_get_ptr(gff->groups, i);
    for (j = 0; j < lst_size(group->features); j++)
      lst_push_ptr(gff->features, lst_get_ptr(group->features, j));
  }

  lst_free(starts);
  lst_free(ends);
  lst_free(scores);
  lst_free(discards);
}

/** Adjust coords of CDS features such that start codons are included
    and stop codons are excluded, as required in GTF2.  Assumes GFF is
    grouped such that at most one start codon and at most one stop
    codon occur per group. */
void gff_fix_start_stop(GFF_Set *gff) {
  int i, j;

  if (gff->groups == NULL) die("ERROR: gff_fix_start_stop requires groups.\n");

  for (i = 0; i < lst_size(gff->groups); i++) {
    GFF_Feature *f, *start = NULL, *stop = NULL;
    GFF_FeatureGroup *g = lst_get_ptr(gff->groups, i);
    /* first scan for start and/or stop codon */
    for (j = 0; j < lst_size(g->features); j++) {
      f = lst_get_ptr(g->features, j);
      if (str_equals_charstr(f->feature, GFF_START_TYPE)) start = f;
      else if (str_equals_charstr(f->feature, GFF_STOP_TYPE)) stop = f;
    }
    /* now adjust corresponding CDS's */
    if (start != NULL || stop != NULL) {
      for (j = 0; j < lst_size(g->features); j++) {
        f = lst_get_ptr(g->features, j);
        if (str_equals_charstr(f->feature, GFF_CDS_TYPE)) {
          if (start != NULL) {
            if (f->strand == '+' && f->start == start->end + 1) 
              f->start = start->start;
            else if (f->strand == '-' && f->end == start->start - 1)
              f->end = start->end;
          }
          if (stop != NULL) {
            if (f->strand == '+' && f->end == stop->end && 
                stop->start - 1 >= f->start) /* don't move if will
                                                make end < start */
              f->end = stop->start - 1; 
            else if (f->strand == '-' && f->start == stop->start &&
                     stop->end + 1 <= f->end) /* don't move if will
                                                 make start > end */
              f->start = stop->end + 1; 
          }
        }
      }
    }
  }
}

/** Adjust coords of features of "primary" types (e.g., CDS) to
    include any features of "helper" types (e.g., start_codon).
    Features must be grouped and sorted.  No features are created or
    discarded; only coordinates are changed. */
void gff_absorb_helpers(GFF_Set *feats, List *primary_types, 
                        List *helper_types) {
  int i, j, k;

  if (feats->groups == NULL) 
    die("ERROR: gff_absorb_helpers requires groups.\n");

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);
      if (str_in_list(f->feature, primary_types)) {
        /* extend to left */
        for (k = j-1; k >= 0; k--) {
          GFF_Feature *prev = lst_get_ptr(g->features, k);
          if (str_in_list(prev->feature, helper_types) && 
              prev->end == f->start - 1) {
            f->start = prev->start;
            if (f->strand == '+' && f->frame != GFF_NULL_FRAME)
              f->frame = (f->frame + 2*(prev->end - prev->start + 1)) % 3;
                                /* to subtract x-y in mod-3 space, you
                                   can do (x + 3y - y) % 3 = (x + 2y)
                                   % 3; it's a form of borrowing.
                                   Note that we're assuming a range
                                   size of 3 */
          }
          else break;
        }
        /* extend to right */
        for (k = j+1; k < lst_size(g->features); k++) {
          GFF_Feature *next = lst_get_ptr(g->features, k);
          if (str_in_list(next->feature, helper_types) && 
              next->start == f->end + 1) {
            f->end = next->end;
            if (f->strand == '-' && f->frame != GFF_NULL_FRAME)
              f->frame = (f->frame + 2*(next->end - next->start + 1)) % 3;
          }
          else break;
        }
      }
    }
  }
}

/** Add a gene_id tag, along with whatever other tags are in use.
   Required in output by some programs */
void gff_add_gene_id(GFF_Set *feats) {
  int i, j;
  char tmp[STR_LONG_LEN];
  if (feats->groups == NULL) 
    die("ERROR: gff_apply_gene_id requires groups.\n");
  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);
      sprintf(tmp, "gene_id \"%s\" ; %s", g->name->chars, f->attribute != NULL ?
              f->attribute->chars : "");
      str_cpy_charstr(f->attribute, tmp);
    }
  }
}

/** remove all groups whose names are not in the specified list */
void gff_filter_by_group(GFF_Set *feats, List *groups) {
  int i, j;
  char *tag;
  Hashtable *hash = hsh_new(lst_size(groups)+1);
  List *keepers = lst_new_ptr(lst_size(feats->features)+1);

  if (feats->groups == NULL) 
    die("ERROR: gff_filter_by_group requires groups.\n");

  for (i = 0; i < lst_size(groups); i++) 
    hsh_put_int(hash, ((String*)lst_get_ptr(groups, i))->chars, 1);

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    int keep = FALSE;
    if (hsh_get_int(hash, g->name->chars) != -1) 
      keep = TRUE;
    for (j = 0; j < lst_size(g->features); j++) {
      if (keep)
        lst_push_ptr(keepers, lst_get_ptr(g->features, j));
      else
        gff_free_feature(lst_get_ptr(g->features, j));
    }
  }

  lst_free(feats->features);
  feats->features = keepers;

  tag = strdup(feats->group_tag->chars);
  gff_group(feats, tag);        /* old one will have stale pointers */
  free(tag);

  hsh_free(hash);
}

/** Creates 5'UTR and 3'UTR features for cases in which exon features
    extend beyond cds features of the same group */
void gff_create_utrs(GFF_Set *feats) {
  int i, j;
  List *exons = lst_new_ptr(20);

  if (feats->groups == NULL) 
    die("ERROR: gff_create_utrs requires groups.\n");

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    int cds_start = INFTY, cds_end = -1;
    char strand = '\0';

    /* first scan for exon features, strand, and start/end of cds */
    lst_clear(exons);
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);

      if (str_equals_charstr(f->feature, GFF_CDS_TYPE)) {
        if (f->start < cds_start) cds_start = f->start;
        if (f->end > cds_end) cds_end = f->end;
      }

      else if (str_equals_charstr(f->feature, GFF_EXON_TYPE)) 
        lst_push_ptr(exons, f);

      if (strand == '\0') strand = f->strand;
    }    

    /* now add UTR features for upstream and downstream exons */
    if (cds_end > 0) {
      for (j = 0; j < lst_size(exons); j++) {
        GFF_Feature *f = lst_get_ptr(exons, j);
        GFF_Feature *utr_f;
        if (f->start < cds_start) {
          utr_f = gff_new_feature_copy(f);
          if (utr_f->end >= cds_start) utr_f->end = cds_start - 1;
          str_cpy_charstr(utr_f->feature, strand == '-' ? GFF_UTR3_TYPE : 
                          GFF_UTR5_TYPE);
          lst_push_ptr(feats->features, utr_f);
          lst_push_ptr(g->features, utr_f);
        }
        if (f->end > cds_end) {
          utr_f = gff_new_feature_copy(f);
          if (utr_f->start <= cds_end) utr_f->start = cds_end + 1;
          str_cpy_charstr(utr_f->feature, strand == '-' ? GFF_UTR5_TYPE : 
                          GFF_UTR3_TYPE);
          lst_push_ptr(feats->features, utr_f);
          lst_push_ptr(g->features, utr_f);
        }
      }
    }
  }
  lst_free(exons);
}

/** Creates intron features between exons of the same group */
void gff_create_introns(GFF_Set *feats) {
  int i, j;
  List *exons = lst_new_ptr(20);

  if (feats->groups == NULL) 
    die("ERROR: gff_create_introns requires groups.\n");

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);

    /* first scan for exon features */
    lst_clear(exons);
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);
      if (str_equals_charstr(f->feature, GFF_EXON_TYPE)) 
        lst_push_ptr(exons, f);
    }    

    /* now add intron features between exons */
    lst_qsort(exons, gff_feature_comparator);
    for (j = 0; j < lst_size(exons) - 1; j++) {
      GFF_Feature *exon1 = lst_get_ptr(exons, j);
      GFF_Feature *exon2 = lst_get_ptr(exons, j+1);
      GFF_Feature *intron = gff_new_feature_copy(exon1);
      intron->start = exon1->end + 1;
      intron->end = exon2->start - 1;
      str_cpy_charstr(intron->feature, GFF_INTRON_TYPE);
      lst_push_ptr(feats->features, intron);
      lst_push_ptr(g->features, intron);
    }
  }
  lst_free(exons);
}

/** Creates features for start and stop codons and 5' and 3' splice
    sites.  Note: splice sites in UTR will only be added if UTR is
    annotated (see function above) */

void gff_create_signals(GFF_Set *feats) {
  int i, j;

  if (feats->groups == NULL) 
    die("ERROR: gff_create_signals requires groups.\n");

  for (i = 0; i < lst_size(feats->groups); i++) {
    GFF_FeatureGroup *g = lst_get_ptr(feats->groups, i);
    int cds_start = INFTY, cds_end = -1, trans_start = INFTY, trans_end = -1;
    char strand = '\0';

    /* first scan for strand and start/end of cds and transcript */
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);
      if (str_equals_charstr(f->feature, GFF_CDS_TYPE)) {
        if (f->start < cds_start) cds_start = f->start;
        if (f->end > cds_end) cds_end = f->end;
      }
      if (str_equals_charstr(f->feature, GFF_CDS_TYPE) ||
          str_equals_charstr(f->feature, GFF_UTR5_TYPE) ||
          str_equals_charstr(f->feature, GFF_UTR3_TYPE)) {
        if (f->start < trans_start) trans_start = f->start;
        if (f->end > trans_end) trans_end = f->end;
      }
      if (strand == '\0') strand = f->strand;
    }    

    /* now add signal features */
    for (j = 0; j < lst_size(g->features); j++) {
      GFF_Feature *f = lst_get_ptr(g->features, j);
      GFF_Feature *f_new;
      if (str_equals_charstr(f->feature, GFF_CDS_TYPE) &&
          f->end - f->start + 1 >= 3) {
                                /* don't create if shorter than 3 b.p. */
        if (f->start == cds_start) {
          f_new = gff_new_feature_copy(f);
          f_new->end = f->start + 2;
          if (strand == '-') {
            str_cpy_charstr(f_new->feature, GFF_STOP_TYPE);
            f->start += 3;
            f_new->frame = (f->frame + f->end - f->start + 1) % 3;
          }
          else 
            str_cpy_charstr(f_new->feature, GFF_START_TYPE);            
          lst_push_ptr(feats->features, f_new);
          lst_push_ptr(g->features, f_new);
        }
        if (f->end == cds_end) {
          f_new = gff_new_feature_copy(f);
          f_new->start = f->end - 2;
          if (strand == '-') 
            str_cpy_charstr(f_new->feature, GFF_START_TYPE);
          else {
            str_cpy_charstr(f_new->feature, GFF_STOP_TYPE);            
            f->end -= 3;
            f_new->frame = (f->frame + f->end - f->start + 1) % 3;
          }
          lst_push_ptr(feats->features, f_new);
          lst_push_ptr(g->features, f_new);
        }
      }

      if ((str_equals_charstr(f->feature, GFF_CDS_TYPE) && 
           f->start != cds_start && f->start != cds_start + 3) || 
          ((str_equals_charstr(f->feature, GFF_UTR5_TYPE) ||
            str_equals_charstr(f->feature, GFF_UTR3_TYPE)) &&
           f->start != trans_start && f->start != cds_end + 1)) { 
        /* add splice site before exon */
          f_new = gff_new_feature_copy(f);
          f_new->end = f->start - 1;
          f_new->start = f_new->end - 1;
          str_cpy_charstr(f_new->feature, strand == '-' ? GFF_SPLICE5_TYPE : 
                          GFF_SPLICE3_TYPE);
          lst_push_ptr(feats->features, f_new);
          lst_push_ptr(g->features, f_new);
      }

      if ((str_equals_charstr(f->feature, GFF_CDS_TYPE) && 
           f->end != cds_end && f->end != cds_end - 3) || 
          ((str_equals_charstr(f->feature, GFF_UTR5_TYPE) ||
            str_equals_charstr(f->feature, GFF_UTR3_TYPE)) &&
           f->end != cds_start - 1 && f->end != trans_end)) { 
        /* add splice site after exon */
          f_new = gff_new_feature_copy(f);
          f_new->start = f->end + 1;
          f_new->end = f_new->start + 1;
          str_cpy_charstr(f_new->feature, strand == '-' ? GFF_SPLICE3_TYPE : 
                          GFF_SPLICE5_TYPE);
          lst_push_ptr(feats->features, f_new);
          lst_push_ptr(g->features, f_new);
      }
    }    
  }
}

/* return index of group that feature f belongs to.  If pos is not
   NULL, sets it to feature's position in group.
   Warning: not efficient- must search through all groups until
   feature is found */
int gff_group_idx(GFF_Set *feats, GFF_Feature *f, int *pos) {
  GFF_FeatureGroup *grp;
  int i, j;
  if (pos != NULL) *pos = -1;
  if (feats->groups == NULL) return -1;
  for (i=0; i<lst_size(feats->groups); i++) {
    grp = (GFF_FeatureGroup*)lst_get_ptr(feats->groups, i);
    for (j=0; j<lst_size(grp->features); j++) {
      if ((GFF_Feature*)lst_get_ptr(grp->features, j) == f) {
	if (pos != NULL) *pos = j;
	return i;
      }
    }
  }
  die("ERROR: gff_group_idx couldn't find feature in any group\n");
  return -1;
}

/* return name of group that feature f belongs to.
   Warning: not efficient- must search through all groups until
   feature is found */
String *gff_group_name(GFF_Set *feats, GFF_Feature *f) {
  int idx = gff_group_idx(feats, f, NULL);
  if (idx == -1) return NULL;
  return ((GFF_FeatureGroup*)lst_get_ptr(feats->groups, idx))->name;
}


/** Sub-routine called by gff_flatten and gff_flatten_groups.
    Merges overlapping or adjacent features of same type.  Assumes
    features are sorted.  When two features are merged, scores are
    summed, but attributes are ignored.  Will not merge if 'frame' is
    non-null.  If keepGroups, will not combine features in different
    groups, and will preserve group structure*/
void gff_flatten_sub(GFF_Set *feats, int keepGroups) {
  List *keepers;
  GFF_Feature *last;
  int i, changed = FALSE;

  if (lst_size(feats->features) <= 1) return;

  keepers = lst_new_ptr(lst_size(feats->features));
  last = lst_get_ptr(feats->features, 0);
  lst_push_ptr(keepers, last);

  for (i = 1; i < lst_size(feats->features); i++) {
    GFF_Feature *this = lst_get_ptr(feats->features, i);
    if (last->end >= this->start - 1 && last->strand == this->strand && 
	str_equals(last->feature, this->feature) && 
	last->frame == GFF_NULL_FRAME && this->frame == GFF_NULL_FRAME) {
      //need to see if two features are in same group.  Unfortunately 
      //no efficient way to do this
      if (keepGroups && feats->groups != NULL) {
	int thisGrp, lastGrp, thispos;
	thisGrp = gff_group_idx(feats, this, &thispos);
	lastGrp = gff_group_idx(feats, last, NULL);

	//don't combine if this/last are in different groups
	if (thisGrp != lastGrp) continue;

	//otherwise need to remove this from group list
	lst_delete_idx(lst_get_ptr(feats->groups, thisGrp), thispos);
      }
      last->end = this->end;
      if (!last->score_is_null && !this->score_is_null) 
	last->score += this->score;
      /* (ignore attribute) */
      gff_free_feature(this);
      changed = TRUE;
    }
    else {
      lst_push_ptr(keepers, this);
      last = this;
    }
  }
  if (changed) {
    lst_free(feats->features);
    feats->features = keepers;
    if (feats->groups != NULL && keepGroups==0) 
      gff_ungroup(feats);
  }
  else 
    lst_free(keepers);
}


/** Merges overlapping or adjacent features of same type.  Assumes
    features are sorted.  When two features are merged, scores are
    summed, but attributes are ignored.  Will not merge if 'frame' is
    non-null.  Removes group structure and combines between groups if
    they are defined */
void gff_flatten(GFF_Set *feats) {
  gff_flatten_sub(feats, 0);
}


/** Merges overlapping or adjacent features of same type, if they
    are they are in the same group.  Assumes features are sorted.  
    When two features are merged, scores are summed, but attributes are 
    ignored.  Will not merge if 'frame' is non-null.  */
void gff_flatten_within_groups(GFF_Set *feats) {
  List *keepers;
  GFF_Feature *last;
  int i, changed = FALSE;

  if (lst_size(feats->features) <= 1) return;

  keepers = lst_new_ptr(lst_size(feats->features));
  last = lst_get_ptr(feats->features, 0);
  lst_push_ptr(keepers, last);

  for (i = 1; i < lst_size(feats->features); i++) {
    GFF_Feature *this = lst_get_ptr(feats->features, i);
    if (last->end >= this->start - 1 && last->strand == this->strand && 
	str_equals(last->feature, this->feature) && 
	last->frame == GFF_NULL_FRAME && this->frame == GFF_NULL_FRAME) {
      last->end = this->end;
      if (!last->score_is_null && !this->score_is_null) 
	last->score += this->score;
      /* (ignore attribute) */
      gff_free_feature(this);
      changed = TRUE;
    }
    else {
      lst_push_ptr(keepers, this);
      last = this;
    }
  }
  if (changed) {
    lst_free(feats->features);
    feats->features = keepers;
    if (feats->groups != NULL) gff_ungroup(feats);
  }
  else 
    lst_free(keepers);
}


/* partition features of a GFF by feature type.  On return, 'types'
   will be a list of feature types and subsets will be a corresponding
   list of lists of features */
void gff_partition_by_type(GFF_Set *feats, List *types, List *subsets) {
  int i, idx;
  lst_clear(types);
  lst_clear(subsets);
  for (i = 0; i < lst_size(feats->features); i++) {
    GFF_Feature *f = lst_get_ptr(feats->features, i);
    if (!str_in_list_idx(f->feature, types, &idx)) {
      lst_push_ptr(types, f->feature);
      lst_push_ptr(subsets, lst_new_ptr(max(1, lst_size(feats->features) / 10)));
      idx = lst_size(types) - 1;
    }
    lst_push_ptr(lst_get_ptr(subsets, idx), f);
  }
}

/* Clear all features from a GFF_Set and free associated memory */
void gff_clear_set(GFF_Set *gff) {
  int i;
  GFF_Feature *f;
  for (i = 0; i < lst_size(gff->features); i++) {
    f = lst_get_ptr(gff->features, i);
    gff_free_feature(f);
  }
  lst_clear(gff->features);
  if (gff->groups != NULL) gff_ungroup(gff);
}


/* adds offset to start and end of each feature. */
/* If new end coordinate < 1, or if maxCoord > 0 and new
   start coordinate > maxCoord, then feature is removed.
   If part of feature is out of bounds, start and end
   may be truncated to be in the range [1,maxCoord] (or
   [1, infinity) if maxCoord < 0 */
void gff_add_offset(GFF_Set *gff, int offset, int maxCoord) {
  List *keepers = lst_new_ptr(lst_size(gff->features));
  GFF_Feature *feat;
  int i;
  for (i=0; i < lst_size(gff->features); i++) {
    feat = (GFF_Feature*)lst_get_ptr(gff->features, i);
    feat->start += offset;
    feat->end += offset;
    if (feat->end < 1 || (maxCoord > 0 && feat->start > maxCoord)) {
      gff_free_feature(feat);
    } else {
      if (feat->start < 1) feat->start = 1;
      if (maxCoord > 0 && feat->end > maxCoord) feat->end = maxCoord;
      lst_push_ptr(keepers, feat);
    }
  }
  lst_free(gff->features);
  gff->features = keepers;
  if (gff->groups != NULL) gff_ungroup(gff);
}
