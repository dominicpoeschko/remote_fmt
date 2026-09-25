/* remote_fmt's call-site tags in one section, within 15 bits of siteAnchor */
SECTIONS
{
    remote_fmt_sites : { KEEP(*(remote_fmt_sites remote_fmt_sites.*)) }
}
INSERT AFTER .data;
