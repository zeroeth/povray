/*******************************************************************************
 * gif.cpp
 *
 * This module contains the code to read the GIF file format.
 *
 * NOTE:  Portions of this module were written by Steve Bennett and are used
 *        here with his permission.
 *
 * ---------------------------------------------------------------------------
 * Persistence of Vision Ray Tracer ('POV-Ray') version 3.7.
 * Copyright 1991-2013 Persistence of Vision Raytracer Pty. Ltd.
 *
 * POV-Ray is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * POV-Ray is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *********************************************************************************/

/*
 * The following routines were borrowed freely from FRACTINT, and represent
 * a generalized GIF file decoder.
 *
 * Swiped and converted to entirely "C" coded routines by AAC for the most
 * in future portability!
 */

#include <vector>

// configbase.h must always be the first POV file included within base *.cpp files
#include "base/configbase.h"
#include "base/image/image.h"
#include "base/image/gif.h"

#include <boost/thread.hpp>

// this must be the last file included
#include "base/povdebug.h"

namespace pov_base
{

namespace Gif
{

// TODO: make sure we don't leak an image object if we throw an exception.
Image *Read (IStream *file, const Image::ReadOptions& options, bool IsPOTFile)
{
	int                             data ;
	int                             width;
	int                             height;
	Image                           *image = NULL;
	unsigned char                   buffer[256];
	vector<Image::RGBAMapEntry>     colormap ;
	int                             alphaIdx = -1; // assume no transparency color

	// GIF files used to have no clearly defined gamma by default, but a W3C recommendation exists for them to use sRGB.
	// Anyway, use whatever the user has chosen as default.
	GammaCurvePtr gamma;
	if (options.gammacorrect)
	{
		if (options.defaultGamma)
			gamma = TranscodingGammaCurve::Get(options.workingGamma, options.defaultGamma);
		else
			gamma = TranscodingGammaCurve::Get(options.workingGamma, SRGBGammaCurve::Get());
	}

	int status = 0;

	/* Get the screen description. */
	if (!file->read (buffer, 13))
		throw POV_EXCEPTION(kFileDataErr, "Cannot read GIF file header");

	/* Use updated GIF specs. */
	if (memcmp (buffer, "GIF", 3) != 0)
		throw POV_EXCEPTION(kFileDataErr, "File is not in GIF format");

	if (buffer[3] != '8' || (buffer[4] != '7' && buffer[4] != '9') || buffer[5] < 'A' || buffer[5] > 'z')
		throw POV_EXCEPTION(kFileDataErr, "Unsupported GIF version");

	int planes = ((unsigned) buffer [10] & 0x0F) + 1;
	int colourmap_size = (1 << planes);

	/* Color map (better be!) */
	if ((buffer[10] & 0x80) == 0)
		throw POV_EXCEPTION(kFileDataErr, "Error in GIF color map");

	for (int i = 0; i < colourmap_size ; i++)
	{
		Image::RGBAMapEntry entry;
		if (!file->read (buffer, 3))
			throw POV_EXCEPTION(kFileDataErr, "Cannot read GIF colormap");
		entry.red   = IntDecode(gamma, buffer[0], 255);
		entry.green = IntDecode(gamma, buffer[1], 255);
		entry.blue  = IntDecode(gamma, buffer[2], 255);
		entry.alpha = 1.0f;
		colormap.push_back(entry);
	}

	/* Now read one or more GIF objects. */
	bool finished = false;

	while (!finished)
	{
		switch (file->Read_Byte())
		{
			case EOF:
				throw POV_EXCEPTION(kFileDataErr, "Unexpected EOF reading GIF file");
				finished = true;
				break ;

			case ';': /* End of the GIF dataset. */
				finished = true;
				status = 0;
				break;

			case '!': /* GIF Extension Block. */
				/* Read (and check) the ID. */
				if (file->Read_Byte() == 0xF9)
				{
					if ((data = file->Read_Byte()) > 0)
					{
						if (!file->read (buffer, data))
							throw POV_EXCEPTION(kFileDataErr, "Unexpected EOF reading GIF file");
						// check transparency flag, and set transparency color index if appropriate
						if (data >= 3 && buffer[0] & 0x01)
						{
							int alphaIdx = buffer[3];
							if (alphaIdx < colourmap_size)
								colormap[alphaIdx].alpha = 0.0f;
						}
					}
					else
						break;
				}
				while ((data = file->Read_Byte()) > 0)
					if (!file->read (buffer, data))
						throw POV_EXCEPTION(kFileDataErr, "Unexpected EOF reading GIF file");
				break;

			case ',': /* Start of image object. Get description. */
				for (int i = 0; i < 9; i++)
				{
					if ((data = file->Read_Byte()) == EOF)
						throw POV_EXCEPTION(kFileDataErr, "Unexpected EOF reading GIF file");
					buffer[i] = (unsigned char) data;
				}

				/* Check "interlaced" bit. */
				if ((buffer[9] & 0x40) != 0)
					throw POV_EXCEPTION(kFileDataErr, "Interlacing in GIF image unsupported");

				width  = (int) buffer[4] | ((int) buffer[5] << 8);
				height = (int) buffer[6] | ((int) buffer[7] << 8);
				image = Image::Create (width, height, Image::Colour_Map, colormap) ;
				// [CLi] GIF only uses full opacity or full transparency, so premultiplied vs. non-premultiplied alpha is not an issue

				/* Get bytes */
				Decode (file, image);
				finished = true;
				break;

			default:
				status = -1;
				finished = true;
				break;
		}
	}

	if (!image)
		throw POV_EXCEPTION(kFileDataErr, "Cannot find GIF image data block");

	if (IsPOTFile == false)
		return (image);

	// POT files are GIF files where the right half of the image contains
	// a second byte for each pixel on the left, thus allowing 16-bit
	// indexes. In this case the palette data is ignored and we convert
	// the image into a 16-bit grayscale version.
	if ((width & 0x01) != 0)
		throw POV_EXCEPTION(kFileDataErr, "Invalid width for POT file");
	int newWidth = width / 2 ;
	Image *newImage = Image::Create (newWidth, height, Image::Gray_Int16) ;
	for (int y = 0 ; y < height ; y++)
		for (int x = 0 ; x < newWidth ; x++)
			newImage->SetGrayValue (x, y, (unsigned int) image->GetIndexedValue (x, y) << 8 | image->GetIndexedValue (x + newWidth, y)) ;
			// NB: POT files don't use alpha, so premultiplied vs. non-premultiplied is not an issue
			// NB: No gamma adjustment happening here!
	delete image ;
	return (newImage) ;
}

} // end of namespace Gif

}

