<?xml version="1.0" encoding="UTF-8" ?>
<xsl:stylesheet version="1.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform">
  <!-- Extract a list of newline-separated link targets from html -->
  <xsl:output method="text"/>

  
  <xsl:template match="a[@href]">
    <xsl:text>&#xA;</xsl:text>
    <xsl:value-of select='@href'/>
  </xsl:template>

  <xsl:template match="script|noscript"/>

  <xsl:template match="@*|node()">
    <xsl:apply-templates/>
  </xsl:template>

</xsl:stylesheet>
