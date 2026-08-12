// This file Copyright © Transmission authors and contributors.
// It may be used under the MIT (SPDX: MIT) license.
// License text can be found in the licenses/ folder.

#import "TorrentCell.h"
#import "ProgressBarView.h"
#import "ProgressGradients.h"
#import "Torrent.h"

@implementation TorrentCell

- (void)drawRect:(NSRect)dirtyRect
{
    if (self.fTorrentTableView) {
        Torrent* torrent = (Torrent*)self.objectValue;

        // draw progress bar
        NSRect barRect = self.fTorrentProgressBarView.frame;
        [ProgressBarView.sharedInstance drawBarInRect:barRect forTableView:self.fTorrentTableView withTorrent:torrent];

        // set priority icon
        if (torrent.priority != TR_PRI_NORMAL) {
            NSImage* priorityImage = [NSImage imageNamed:(torrent.priority == TR_PRI_HIGH ? @"PriorityHighTemplate" : @"PriorityLowTemplate")];

            self.fTorrentPriorityView.image = priorityImage;

            [self.fStackView setVisibilityPriority:NSStackViewVisibilityPriorityMustHold forView:self.fTorrentPriorityView];
        } else {
            [self.fStackView setVisibilityPriority:NSStackViewVisibilityPriorityNotVisible forView:self.fTorrentPriorityView];
        }
    }

    [super drawRect:dirtyRect];
}

// otherwise progress bar is inverted
- (BOOL)isFlipped
{
    return YES;
}

- (void)setBackgroundStyle:(NSBackgroundStyle)backgroundStyle
{
    [super setBackgroundStyle:backgroundStyle];

    NSColor* priorityColor = backgroundStyle == NSBackgroundStyleEmphasized ? NSColor.whiteColor : NSColor.labelColor;
    self.fTorrentPriorityView.contentTintColor = priorityColor;
}

@end
